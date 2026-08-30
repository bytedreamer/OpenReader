#include "rc522.h"
#include "board.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include <string.h>

static const char *TAG = "rc522";

/* ---- Register map (datasheet section 9.2) -------------------------------
 * Addresses are given here already shifted into the position the SPI
 * address byte wants, which is how the datasheet's own SPI section frames
 * it: bit 7 selects read/write, bits 6..1 are the address, bit 0 is 0. */
#define REG_COMMAND        (0x01U << 1)
#define REG_COM_IRQ        (0x04U << 1)
#define REG_DIV_IRQ        (0x05U << 1)
#define REG_ERROR          (0x06U << 1)
#define REG_FIFO_DATA      (0x09U << 1)
#define REG_FIFO_LEVEL     (0x0AU << 1)
#define REG_CONTROL        (0x0CU << 1)
#define REG_BIT_FRAMING    (0x0DU << 1)
#define REG_COLL           (0x0EU << 1)
#define REG_MODE           (0x11U << 1)
#define REG_TX_CONTROL     (0x14U << 1)
#define REG_TX_ASK         (0x15U << 1)
#define REG_CRC_RESULT_H   (0x21U << 1)
#define REG_CRC_RESULT_L   (0x22U << 1)
#define REG_T_MODE         (0x2AU << 1)
#define REG_T_PRESCALER    (0x2BU << 1)
#define REG_T_RELOAD_H     (0x2CU << 1)
#define REG_T_RELOAD_L     (0x2DU << 1)
#define REG_VERSION        (0x37U << 1)

/* ---- PCD commands ------------------------------------------------------- */
#define CMD_IDLE           0x00U
#define CMD_CALC_CRC       0x03U
#define CMD_TRANSCEIVE     0x0CU
#define CMD_SOFT_RESET     0x0FU

/* ---- PICC commands (ISO/IEC 14443-3) ------------------------------------ */
#define PICC_REQA          0x26U
#define PICC_HLTA          0x50U
#define PICC_SEL_CL1       0x93U
#define PICC_SEL_CL2       0x95U
#define PICC_SEL_CL3       0x97U

/* A cascade level returns this as its first UID byte when the real UID is
 * longer and continues at the next level. */
#define UID_CASCADE_TAG    0x88U

/* ---- Bus ---------------------------------------------------------------
 *
 * Every exchange with the MFRC522 is exactly two bytes — an address byte
 * then a data byte — so the whole bus dependency is one function. Which
 * makes the choice between hardware SPI2 and a bit-banged bus a swap of
 * that function rather than a second driver: everything above it, the
 * register map and the 14443-A state machine, is shared.
 *
 * The bit-banged path exists because SPI2 is the C6's only general-purpose
 * SPI master and the LCD cannot be moved off it (board.h). Clocking the
 * RC522 in software is what lets the panel and the reader run together. */

#if CONFIG_OPENREADER_RC522_HW_SPI

static const char *const BUS_NAME = "hardware SPI2";

static spi_device_handle_t s_spi;

static esp_err_t xfer2(const uint8_t tx[2], uint8_t rx[2])
{
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t bus_init(void)
{
    /* 5 MHz: comfortably inside the MFRC522's 10 MHz ceiling and slow
     * enough to tolerate the jumper wires most people will use. */
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 5 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = BOARD_RC522_CS,
        .queue_size     = 1,
    };
    return spi_bus_add_device(BOARD_SPI_HOST, &dev, &s_spi);
}

#else /* CONFIG_OPENREADER_RC522_SOFT_SPI */

static const char *const BUS_NAME = "bit-banged SPI";

/* Half a clock period. One microsecond puts the bus near 500 kHz — two
 * orders of magnitude under the part's ceiling, and deliberately so: these
 * are jumper wires, and the entire cost is a few dozen register accesses
 * ten times a second. Raising the rate would buy microseconds nobody is
 * waiting for. */
#define SOFT_SPI_HALF_US 1

/* SPI mode 0, MSB first: clock idles low, the slave samples MOSI on the
 * rising edge, and the master samples MISO on that same edge. */
static uint8_t soft_xfer_byte(uint8_t out)
{
    uint8_t in = 0;
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(BOARD_RC522_MOSI, (out >> bit) & 1U);
        esp_rom_delay_us(SOFT_SPI_HALF_US);
        gpio_set_level(BOARD_RC522_SCLK, 1);
        if (gpio_get_level(BOARD_RC522_MISO)) {
            in |= (uint8_t)(1U << bit);
        }
        esp_rom_delay_us(SOFT_SPI_HALF_US);
        gpio_set_level(BOARD_RC522_SCLK, 0);
    }
    return in;
}

static esp_err_t xfer2(const uint8_t tx[2], uint8_t rx[2])
{
    /* CS framing is ours here, where the hardware path gets it from
     * spics_io_num. The MFRC522 latches the address on the first byte of a
     * transaction, so CS must stay low across both bytes — dropping it
     * between them would restart the address phase. */
    gpio_set_level(BOARD_RC522_CS, 0);
    esp_rom_delay_us(SOFT_SPI_HALF_US);

    uint8_t b0 = soft_xfer_byte(tx[0]);
    uint8_t b1 = soft_xfer_byte(tx[1]);

    esp_rom_delay_us(SOFT_SPI_HALF_US);
    gpio_set_level(BOARD_RC522_CS, 1);

    if (rx != NULL) {
        rx[0] = b0;
        rx[1] = b1;
    }
    return ESP_OK;
}

static esp_err_t bus_init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_RC522_SCLK) |
                        (1ULL << BOARD_RC522_MOSI) |
                        (1ULL << BOARD_RC522_CS),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "soft spi outputs");

    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << BOARD_RC522_MISO,
        .mode         = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "soft spi miso");

    /* Idle state before anything is clocked: CS released, clock low. */
    gpio_set_level(BOARD_RC522_CS, 1);
    gpio_set_level(BOARD_RC522_SCLK, 0);
    return ESP_OK;
}

#endif

/* ---- Register access ---------------------------------------------------- */

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t tx[2] = { (uint8_t)(reg & 0x7EU), val };
    return xfer2(tx, NULL);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    const uint8_t tx[2] = { (uint8_t)(0x80U | (reg & 0x7EU)), 0x00U };
    uint8_t rx[2] = { 0 };
    esp_err_t err = xfer2(tx, rx);
    if (err == ESP_OK) {
        *val = rx[1];
    }
    return err;
}

static esp_err_t reg_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v = 0;
    esp_err_t err = reg_read(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    return reg_write(reg, (uint8_t)(v | mask));
}

static esp_err_t reg_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v = 0;
    esp_err_t err = reg_read(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    return reg_write(reg, (uint8_t)(v & (uint8_t)~mask));
}

/* ---- CRC_A -------------------------------------------------------------- */

/* The MFRC522 computes CRC_A in hardware. Doing it on the ESP32 instead
 * would be easy, but the chip's own engine is what the datasheet timing
 * assumes and it keeps FIFO handling in one place. */
static esp_err_t calc_crc(const uint8_t *data, size_t len, uint8_t out[2])
{
    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_IDLE), TAG, "idle");
    ESP_RETURN_ON_ERROR(reg_write(REG_DIV_IRQ, 0x04U), TAG, "clear crcirq");
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_FIFO_LEVEL, 0x80U), TAG, "flush");

    for (size_t i = 0; i < len; i++) {
        ESP_RETURN_ON_ERROR(reg_write(REG_FIFO_DATA, data[i]), TAG, "fifo");
    }
    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_CALC_CRC), TAG, "crc");

    /* The engine finishes in well under a millisecond for the <=16-byte
     * frames we ever CRC; 5 ms of polling is a generous ceiling. */
    for (int i = 0; i < 50; i++) {
        uint8_t irq = 0;
        ESP_RETURN_ON_ERROR(reg_read(REG_DIV_IRQ, &irq), TAG, "divirq");
        if (irq & 0x04U) {  /* CRCIRq */
            ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_IDLE), TAG, "idle");
            ESP_RETURN_ON_ERROR(reg_read(REG_CRC_RESULT_L, &out[0]),
                                TAG, "crcl");
            ESP_RETURN_ON_ERROR(reg_read(REG_CRC_RESULT_H, &out[1]),
                                TAG, "crch");
            return ESP_OK;
        }
        esp_rom_delay_us(100);
    }
    return ESP_ERR_TIMEOUT;
}

/* ---- Transceive --------------------------------------------------------- */

/* Send `send_len` bytes and collect the card's answer.
 *
 * `tx_last_bits` is how many bits of the final TX byte are valid (0 means
 * all eight) — the anticollision loop sends partial bytes, which is the
 * whole reason BitFramingReg exists. `rx_align` places the first received
 * bit, and on return `*rx_last_bits` reports how many bits of the last RX
 * byte are valid. */
static esp_err_t transceive(const uint8_t *send, size_t send_len,
                            uint8_t tx_last_bits, uint8_t rx_align,
                            uint8_t *recv, size_t *recv_len,
                            uint8_t *rx_last_bits)
{
    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_IDLE), TAG, "idle");
    ESP_RETURN_ON_ERROR(reg_write(REG_COM_IRQ, 0x7FU), TAG, "clear irq");
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_FIFO_LEVEL, 0x80U), TAG, "flush");

    for (size_t i = 0; i < send_len; i++) {
        ESP_RETURN_ON_ERROR(reg_write(REG_FIFO_DATA, send[i]), TAG, "fifo");
    }

    ESP_RETURN_ON_ERROR(
        reg_write(REG_BIT_FRAMING,
                  (uint8_t)(((rx_align & 0x07U) << 4) | (tx_last_bits & 0x07U))),
        TAG, "framing");
    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_TRANSCEIVE), TAG, "xceive");
    /* StartSend in BitFramingReg is what actually launches the
     * transmission; writing CommandReg alone only arms it. */
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_BIT_FRAMING, 0x80U), TAG, "start");

    /* Wait for RxIRq (0x20) or IdleIRq (0x10), or for the chip's own timer
     * (TimerIRq, 0x01) to fire saying the card never answered. The timer is
     * configured in rc522_init() for roughly 25 ms. */
    uint8_t irq  = 0;
    bool    done = false;
    for (int i = 0; i < 400; i++) {
        ESP_RETURN_ON_ERROR(reg_read(REG_COM_IRQ, &irq), TAG, "comirq");
        if (irq & 0x30U) {
            done = true;
            break;
        }
        if (irq & 0x01U) {
            /* Timer expired: nothing answered. Overwhelmingly the normal
             * case when no card is on the antenna, so it is not logged. */
            (void)reg_clear_bits(REG_BIT_FRAMING, 0x80U);
            return ESP_ERR_NOT_FOUND;
        }
        esp_rom_delay_us(100);
    }
    ESP_RETURN_ON_ERROR(reg_clear_bits(REG_BIT_FRAMING, 0x80U), TAG, "stop");

    if (!done) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t err_reg = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_ERROR, &err_reg), TAG, "error");
    /* BufferOvfl | ParityErr | ProtocolErr. CollErr is deliberately absent
     * from this mask: the anticollision loop expects collisions and reads
     * CollReg itself to resolve them. */
    if (err_reg & 0x13U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t level = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_FIFO_LEVEL, &level), TAG, "level");
    if ((size_t)level > *recv_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint8_t i = 0; i < level; i++) {
        ESP_RETURN_ON_ERROR(reg_read(REG_FIFO_DATA, &recv[i]), TAG, "read");
    }
    *recv_len = level;

    uint8_t control = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CONTROL, &control), TAG, "control");
    if (rx_last_bits != NULL) {
        *rx_last_bits = control & 0x07U;
    }

    return ESP_OK;
}

/* ---- Card discovery ----------------------------------------------------- */

/* REQA is a 7-bit frame, so tx_last_bits is 7. A card in IDLE answers with
 * a 2-byte ATQA. */
static esp_err_t request_a(void)
{
    uint8_t cmd = PICC_REQA;
    uint8_t atqa[2];
    size_t  atqa_len = sizeof(atqa);
    uint8_t rx_bits  = 0;

    /* Collisions during REQA are expected with several cards present and
     * are resolved by the anticollision loop, so ValuesAfterColl has to be
     * cleared here rather than left set from a previous round. */
    ESP_RETURN_ON_ERROR(reg_clear_bits(REG_COLL, 0x80U), TAG, "coll");

    esp_err_t err = transceive(&cmd, 1, 7, 0, atqa, &atqa_len, &rx_bits);
    if (err != ESP_OK) {
        return err;
    }
    if (atqa_len != 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* One cascade level of the ISO/IEC 14443-3 anticollision + SELECT.
 *
 * A single anticollision round gathers the level's four UID bytes, then a
 * full 32-bit SELECT confirms them. That handles one card in the field,
 * which is what a door reader wants; two cards presented at once produce a
 * collision the caller sees as an error rather than an arbitrary winner.
 * Failing is the safer behaviour for an access reader — silently reporting
 * one of two credentials is worse than reporting none. */
static esp_err_t select_cascade(uint8_t sel_cmd, uint8_t uid_part[5],
                                uint8_t *sak)
{
    /* NVB = 0x20 means "no UID bits supplied yet, send me all of them". */
    uint8_t anticoll[2] = { sel_cmd, 0x20U };
    size_t  got_len = 5;
    uint8_t rx_bits = 0;

    esp_err_t err = transceive(anticoll, 2, 0, 0, uid_part, &got_len,
                               &rx_bits);
    if (err != ESP_OK) {
        return err;
    }
    if (got_len != 5) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Byte 4 is BCC, the XOR of the four UID bytes. Checking it costs
     * nothing and catches a marginal antenna before a corrupted UID reaches
     * the ACU as a credential. */
    uint8_t bcc = (uint8_t)(uid_part[0] ^ uid_part[1] ^ uid_part[2]
                            ^ uid_part[3]);
    if (bcc != uid_part[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t collision = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_COLL, &collision), TAG, "coll");
    if ((collision & 0x20U) == 0U) {  /* CollPosNotValid clear => collided */
        return ESP_ERR_INVALID_STATE;
    }

    /* SELECT: the full 7-byte frame (cmd, NVB=0x70, 4 UID bytes, BCC) plus
     * CRC_A. The answer is a 1-byte SAK plus its own CRC_A. */
    uint8_t sel[9] = { sel_cmd, 0x70U,
                       uid_part[0], uid_part[1], uid_part[2], uid_part[3],
                       uid_part[4], 0, 0 };
    ESP_RETURN_ON_ERROR(calc_crc(sel, 7, &sel[7]), TAG, "sel crc");

    uint8_t resp[3];
    size_t  resp_len = sizeof(resp);
    err = transceive(sel, sizeof(sel), 0, 0, resp, &resp_len, &rx_bits);
    if (err != ESP_OK) {
        return err;
    }
    if (resp_len != 3) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t crc[2];
    ESP_RETURN_ON_ERROR(calc_crc(resp, 1, crc), TAG, "sak crc");
    if (crc[0] != resp[1] || crc[1] != resp[2]) {
        return ESP_ERR_INVALID_CRC;
    }

    *sak = resp[0];
    return ESP_OK;
}

/* Put the card back to sleep so it stops answering REQA. Without this a
 * card left on the antenna re-selects on every poll — harmless, but it
 * keeps the field busy for nothing. Failure is uninteresting: HLTA is
 * defined to produce no reply, so "no answer" is the success case. */
static void halt_a(void)
{
    uint8_t buf[4] = { PICC_HLTA, 0x00U, 0, 0 };
    if (calc_crc(buf, 2, &buf[2]) != ESP_OK) {
        return;
    }
    uint8_t resp[1];
    size_t  resp_len = sizeof(resp);
    (void)transceive(buf, sizeof(buf), 0, 0, resp, &resp_len, NULL);
}

/* ---- Public API --------------------------------------------------------- */

static esp_err_t antenna_on(void)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_TX_CONTROL, &v), TAG, "txcontrol");
    if ((v & 0x03U) != 0x03U) {
        return reg_set_bits(REG_TX_CONTROL, 0x03U);
    }
    return ESP_OK;
}

/* Why is the bus silent?
 *
 * VersionReg reading 0x00 means MISO never went high during the transfer.
 * That has two very different causes and the internal pull-up tells them
 * apart: enable it and read again.
 *
 *   0xFF now  -> the pull-up wins, so nothing is driving the line. Three
 *               ways to get here, all worth checking: the MISO wire is
 *               open, the module has no power, or CS is not landing on
 *               BOARD_RC522_CS — an unselected MFRC522 keeps its MISO
 *               output disabled, so a wrong CS looks exactly like a cut
 *               wire from this side.
 *   still 0x00 -> something is actively holding the line down: MISO shorted
 *               to ground or to a pin driven low, or the module fed 5 V so
 *               its ESD diodes clamp.
 *
 * The pull-up is left off afterwards either way; this runs only on the
 * failure path, where init is about to give up. */
static void diagnose_silent_bus(void)
{
    gpio_set_pull_mode(BOARD_RC522_MISO, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(1000);

    uint8_t pulled = 0;
    esp_err_t err = reg_read(REG_VERSION, &pulled);

    gpio_set_pull_mode(BOARD_RC522_MISO, GPIO_FLOATING);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "diagnostic read failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGE(TAG, "pins: SCLK=%d MOSI=%d MISO=%d CS=%d RST=%d (%s)",
             BOARD_RC522_SCLK, BOARD_RC522_MOSI, BOARD_RC522_MISO,
             BOARD_RC522_CS, BOARD_RC522_RST,
             BUS_NAME);

    if (pulled == 0xFFU) {
        ESP_LOGE(TAG, "MISO floats to the internal pull-up (0x%02X): nothing "
                      "is driving it. Check the MISO wire to GPIO%d, that "
                      "the module has 3.3 V, and that CS lands on GPIO%d — "
                      "an unselected module leaves MISO high-Z and looks "
                      "identical to a cut wire.",
                      pulled, BOARD_RC522_MISO, BOARD_RC522_CS);
    } else if (pulled == 0x00U) {
        ESP_LOGE(TAG, "MISO stays low against the internal pull-up: "
                      "something drives it low. Look for a short to ground "
                      "on GPIO%d, or the module fed 5 V instead of 3.3 V.",
                      BOARD_RC522_MISO);
    } else {
        ESP_LOGE(TAG, "MISO answered 0x%02X with the pull-up on: the link is "
                      "marginal rather than dead. Suspect long or unshielded "
                      "jumpers, or a bad ground between the boards.", pulled);
    }
}

esp_err_t rc522_init(void)
{
    ESP_RETURN_ON_ERROR(bus_init(), TAG, "bus");

    /* Hard reset via RST when it is wired; the soft reset below covers the
     * case where it is not. */
    if (BOARD_RC522_RST >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << BOARD_RC522_RST,
            .mode         = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "rst gpio");
        gpio_set_level(BOARD_RC522_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(BOARD_RC522_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_SOFT_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Internal timer as the card-response timeout. Prescaler 0x0D3E with a
     * reload of 30 gives roughly 25 ms, the conventional value from the
     * datasheet's own example: long enough for any 14443-A exchange, short
     * enough that polling an empty field costs almost nothing. */
    ESP_RETURN_ON_ERROR(reg_write(REG_T_MODE, 0x8DU), TAG, "tmode");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_PRESCALER, 0x3EU), TAG, "tprescaler");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_RELOAD_H, 0x00U), TAG, "treloadh");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_RELOAD_L, 30U), TAG, "treloadl");
    /* Force100ASK — the modulation 14443-A wants. */
    ESP_RETURN_ON_ERROR(reg_write(REG_TX_ASK, 0x40U), TAG, "txask");
    /* CRC preset 0x6363 (CRC_A). */
    ESP_RETURN_ON_ERROR(reg_write(REG_MODE, 0x3DU), TAG, "mode");

    ESP_RETURN_ON_ERROR(antenna_on(), TAG, "antenna");

    uint8_t version = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_VERSION, &version), TAG, "version");
    /* 0x91/0x92 are the genuine v1.0/v2.0 parts; clones report other values
     * and still work. 0x00 and 0xFF mean the bus is not talking at all —
     * MISO stuck low or high, which is the wiring fault to go look for. */
    if (version == 0x00U || version == 0xFFU) {
        ESP_LOGE(TAG, "no response (VersionReg=0x%02X) — check SPI wiring "
                      "and that the module is powered from 3.3 V", version);
        diagnose_silent_bus();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "MFRC522 online, VersionReg=0x%02X (%s)", version, BUS_NAME);
    return ESP_OK;
}

esp_err_t rc522_poll(rc522_uid_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = request_a();
    if (err != ESP_OK) {
        return err;   /* ESP_ERR_NOT_FOUND for an empty field */
    }

    static const uint8_t levels[3] = { PICC_SEL_CL1, PICC_SEL_CL2,
                                       PICC_SEL_CL3 };
    uint8_t uid[RC522_UID_MAX_BYTES];
    uint8_t uid_len = 0;
    uint8_t sak     = 0;

    for (int level = 0; level < 3; level++) {
        uint8_t part[5];
        err = select_cascade(levels[level], part, &sak);
        if (err != ESP_OK) {
            return err;
        }

        /* SAK bit 2 (0x04) set means "cascade continues": this level's
         * first byte was the cascade tag, not UID, and three real bytes
         * follow it. */
        if ((sak & 0x04U) != 0U) {
            if (part[0] != UID_CASCADE_TAG) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            memcpy(&uid[uid_len], &part[1], 3);
            uid_len = (uint8_t)(uid_len + 3);
        } else {
            memcpy(&uid[uid_len], &part[0], 4);
            uid_len = (uint8_t)(uid_len + 4);
            break;
        }
    }

    halt_a();

    memcpy(out->bytes, uid, uid_len);
    out->len = uid_len;
    out->sak = sak;
    return ESP_OK;
}
