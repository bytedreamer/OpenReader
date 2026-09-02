#include "rc522.h"
#include "board.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

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
#define PICC_RATS          0xE0U

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

    /* The engine clocks one byte per 2.5 us or so, and the longest thing
     * we CRC is a full ISO-DEP frame — 46 bytes, well inside a tenth of a
     * millisecond. 5 ms of polling is a generous ceiling. */
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

/* ---- Response timeout ---------------------------------------------------
 *
 * The MFRC522's internal timer is what decides that a card never answered:
 * it starts when transmission ends and raises TimerIRq when it expires.
 * One tick is 0.5 ms with the prescaler rc522_init() programs, so the
 * reload registers hold twice the timeout in milliseconds.
 *
 * 14443-3 wants this short — every poll of an empty field waits it out in
 * full. ISO-DEP wants it long and variable: a card's frame waiting time can
 * reach 4.9 s, and a card computing a P-256 signature will use a good part
 * of that. So it moves, and the software wait below follows it. */

#define RC522_DEFAULT_TIMEOUT_MS 15U

static uint32_t s_timeout_ms = RC522_DEFAULT_TIMEOUT_MS;

static esp_err_t set_response_timeout_ms(uint32_t ms)
{
    uint32_t ticks = ms * 2U;   /* 0.5 ms per tick */
    if (ticks == 0U) {
        ticks = 1U;
    } else if (ticks > 0xFFFFU) {
        ticks = 0xFFFFU;        /* the timer tops out at ~32.7 s */
    }

    ESP_RETURN_ON_ERROR(reg_write(REG_T_RELOAD_H, (uint8_t)(ticks >> 8)),
                        TAG, "treloadh");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_RELOAD_L, (uint8_t)(ticks & 0xFFU)),
                        TAG, "treloadl");
    s_timeout_ms = ms;
    return ESP_OK;
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
     * (TimerIRq, 0x01) to fire saying the card never answered.
     *
     * Two waiting styles, because the range is now four orders of magnitude
     * wide. A 14443-3 exchange answers in a few hundred microseconds and a
     * task switch would dominate it, so the first 2 ms are spun. Past that
     * we are waiting on a card doing arithmetic — up to its full frame
     * waiting time, which can be seconds — and spinning through that would
     * hold the CPU for no gain, so the loop yields a tick at a time.
     *
     * The chip timer remains the authority on "no answer"; the deadline here
     * only has to outlast it, and reaching it means the MFRC522 itself
     * stopped responding, which is a different fault with a different
     * return. */
    const int64_t start    = esp_timer_get_time();
    const int64_t spin_end = start + 2000;
    const int64_t deadline = start + (int64_t)(s_timeout_ms + 20U) * 1000;

    uint8_t irq  = 0;
    bool    done = false;
    for (;;) {
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

        const int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            break;
        }
        if (now < spin_end) {
            esp_rom_delay_us(100);
        } else {
            vTaskDelay(1);
        }
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
 * defined to produce no reply, so "no answer" is the success case.
 *
 * Reached through rc522_release() rather than from rc522_poll(), because a
 * poll now leaves the card selected for whatever the caller wants to do
 * with it next. */
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

/* ---- ISO/IEC 14443-4 (ISO-DEP) -----------------------------------------
 *
 * The half-duplex block transmission protocol, "T=CL": what turns a card
 * that has been selected into one you can send APDUs to. PKOC needs it, and
 * a MIFARE Classic badge has no idea it exists — which is why the SAK is
 * consulted before any of this runs.
 *
 * Two things here are the MFRC522's doing rather than the protocol's. The
 * frame size we advertise is set by its 64-byte FIFO, and the response
 * timeout has to move with the card's frame waiting time because the chip's
 * timer is what enforces it. Everything else is section 7 of the standard.
 */

/* Frame size this reader accepts, as the FSDI code the RATS parameter byte
 * carries: 4, meaning 48 bytes including the CRC.
 *
 * That number is the FIFO's, not the protocol's. The driver reads the FIFO
 * only after an exchange has completed, so a frame that exactly fills its 64
 * bytes leaves no margin; 48 is the largest standard size comfortably inside
 * it. It costs nothing measurable — a 135-byte PKOC AUTHENTICATE response
 * needs three chained blocks at 45 usable bytes each, and would still need
 * three at 61. */
#define ISO_DEP_FSDI 4U
#define ISO_DEP_FSD  48U

/* PCB (protocol control byte) encodings, ISO/IEC 14443-4 section 7.1. Bits
 * 8-7 name the block type: 00 information, 10 receive-ready, 11 supervisory. */
#define PCB_TYPE_MASK   0xC0U
#define PCB_TYPE_I      0x00U
#define PCB_TYPE_S      0xC0U

#define PCB_I_BLOCK     0x02U   /* 000x xx1x, plus the block number       */
#define PCB_CHAINING    0x10U   /* set in an I-block: more follows        */
#define PCB_BLOCK_NUM   0x01U

#define PCB_R_ACK       0xA2U   /* 1010 x01x - receive-ready, ACK         */
#define PCB_R_MASK      0xF6U   /* the bits that make an R-block an ACK   */

#define PCB_S_WTX       0xF2U
#define PCB_S_KIND_MASK 0x30U   /* 00 deselect, 11 waiting-time extension */
#define PCB_S_WTX_KIND  0x30U
#define PCB_S_DESELECT  0xC2U

/* How long to allow for the ATS. The standard governs it by the default
 * frame waiting time (4.8 ms), but a card that has just powered up and is
 * starting an applet can be slower, and being generous costs nothing here:
 * this is one exchange per card presentation, not per poll. */
#define ISO_DEP_RATS_TIMEOUT_MS 50U

/* Floor and ceiling for a frame waiting time taken from the card's ATS. The
 * ceiling is the standard's own maximum (FWI 14). The floor is ours: a card
 * claiming the 4.8 ms default is within its rights, but this driver measures
 * the wait in 0.5 ms ticks and reaches the register over a bit-banged bus,
 * and giving up early on a slow-but-legal card is a worse failure than
 * waiting 10 ms for nothing. */
#define ISO_DEP_MIN_FWT_MS   10U
#define ISO_DEP_MAX_FWT_MS 4949U

/* One frame waiting time unit, microseconds: 256 x 16 / 13.56 MHz, doubling
 * with every step of FWI and of SFGI alike. */
#define ISO_DEP_FWT_UNIT_US 302U

static struct {
    bool     active;
    uint8_t  block;    /* our block number, 0 or 1, toggled per exchange */
    uint16_t fsc;      /* largest frame the CARD accepts, bytes         */
    uint32_t fwt_ms;   /* how long the card may take to answer          */
} s_dep;

/* Whether a card is currently selected and owed a release. */
static bool s_selected;

/* Exchange one ISO-DEP frame: append CRC_A, transceive, check and strip the
 * CRC. `timeout_ms` is this frame's waiting time — the S(WTX) path passes a
 * multiple of the card's own, which is the entire purpose of that block. */
static esp_err_t dep_frame(const uint8_t *tx, size_t tx_len,
                           uint8_t *rx, size_t rx_cap, size_t *rx_len,
                           uint32_t timeout_ms)
{
    uint8_t out[ISO_DEP_FSD];
    if (tx_len + 2U > sizeof(out)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, tx, tx_len);
    ESP_RETURN_ON_ERROR(calc_crc(out, tx_len, &out[tx_len]), TAG, "tx crc");

    ESP_RETURN_ON_ERROR(set_response_timeout_ms(timeout_ms), TAG, "fwt");

    uint8_t frame[ISO_DEP_FSD];
    size_t  len = sizeof(frame);

    esp_err_t err = transceive(out, tx_len + 2U, 0, 0, frame, &len, NULL);
    if (err != ESP_OK) {
        return err;
    }
    /* A PCB and a CRC_A are the smallest legal frame. */
    if (len < 3U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t crc[2];
    ESP_RETURN_ON_ERROR(calc_crc(frame, len - 2U, crc), TAG, "rx crc");
    if (crc[0] != frame[len - 2U] || crc[1] != frame[len - 1U]) {
        return ESP_ERR_INVALID_CRC;
    }

    len -= 2U;
    if (len > rx_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(rx, frame, len);
    *rx_len = len;
    return ESP_OK;
}

/* Largest INF field we may put in one I-block: the smaller of what the card
 * accepts and what we can frame, less the PCB and the CRC_A. */
static size_t dep_max_inf(void)
{
    uint16_t frame = (s_dep.fsc < ISO_DEP_FSD) ? s_dep.fsc : ISO_DEP_FSD;
    return (size_t)frame - 3U;
}

esp_err_t rc522_iso_dep_activate(void)
{
    /* RATS: E0 followed by (FSDI << 4) | CID. CID 0 is the card's assigned
     * identifier and also the "no CID field in the blocks" convention every
     * card accepts — this reader talks to one card at a time, so there is
     * nothing for a CID to disambiguate. */
    const uint8_t rats[2] = { PICC_RATS, (uint8_t)(ISO_DEP_FSDI << 4) };

    uint8_t ats[ISO_DEP_FSD];
    size_t  ats_len = sizeof(ats);

    memset(&s_dep, 0, sizeof(s_dep));

    /* A card with no ISO-DEP in it does not refuse RATS, it says nothing at
     * all — so ESP_ERR_NOT_FOUND here means either "not a smart card" or
     * "withdrawn mid-transaction", and nothing on the wire separates them.
     * The caller treats both the same way: fall back to the UID. */
    esp_err_t err = dep_frame(rats, sizeof(rats), ats, sizeof(ats), &ats_len,
                              ISO_DEP_RATS_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* TL counts itself and every byte after it, but not the CRC. A card that
     * disagrees with what it actually sent is not one to keep talking to. */
    const uint8_t tl = ats[0];
    if (tl < 1U || (size_t)tl > ats_len) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* The defaults the standard names for an ATS that omits T0: FSC 32
     * bytes, frame waiting time 4.8 ms, no start-up guard time. */
    uint8_t fsci = 2U;
    uint8_t fwi  = 4U;
    uint8_t sfgi = 0U;

    if (tl >= 2U) {
        const uint8_t t0 = ats[1];
        size_t        p  = 2U;

        fsci = (uint8_t)(t0 & 0x0FU);

        /* TA(1) advertises the higher bit rates. Skipped deliberately: this
         * reader sends no PPS and stays at 106 kbit/s, which every card must
         * support and which is never wrong to keep using. */
        if (t0 & 0x10U) {
            p++;
        }
        if (t0 & 0x20U) {              /* TB(1): FWI and SFGI */
            if (p >= (size_t)tl) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            fwi  = (uint8_t)(ats[p] >> 4);
            sfgi = (uint8_t)(ats[p] & 0x0FU);
        }
        /* TC(1) and the historical bytes carry nothing this reader acts on:
         * no CID to negotiate, and PKOC uses no NAD. */
    }

    /* FSCI 9..15 are reserved. Reading one as "large" would risk framing
     * something the card cannot take, so it falls back to the 32-byte
     * default — the direction that costs an extra chained block rather than
     * a failed exchange. */
    static const uint16_t kFsc[] = { 16U, 24U, 32U, 40U, 48U,
                                     64U, 96U, 128U, 256U };
    s_dep.fsc = (fsci < (sizeof(kFsc) / sizeof(kFsc[0]))) ? kFsc[fsci] : 32U;

    if (fwi > 14U) {
        fwi = 14U;
    }
    uint32_t fwt_ms = ((ISO_DEP_FWT_UNIT_US << fwi) / 1000U) + 1U;
    if (fwt_ms < ISO_DEP_MIN_FWT_MS) {
        fwt_ms = ISO_DEP_MIN_FWT_MS;
    } else if (fwt_ms > ISO_DEP_MAX_FWT_MS) {
        fwt_ms = ISO_DEP_MAX_FWT_MS;
    }
    s_dep.fwt_ms = fwt_ms;

    /* The start-up frame guard time: how long the card wants between the ATS
     * and the first command. Ignoring it is the classic reason a card answers
     * RATS and then times out on the very next frame. */
    if (sfgi > 14U) {
        sfgi = 14U;
    }
    const uint32_t sfg_us = ISO_DEP_FWT_UNIT_US << sfgi;
    if (sfg_us < 1000U) {
        esp_rom_delay_us(sfg_us + 100U);
    } else {
        vTaskDelay(pdMS_TO_TICKS(sfg_us / 1000U + 1U));
    }

    s_dep.block  = 0U;
    s_dep.active = true;

    ESP_LOGD(TAG, "ISO-DEP up: card frame %u bytes, FWT %lu ms",
             (unsigned)s_dep.fsc, (unsigned long)s_dep.fwt_ms);
    return ESP_OK;
}

esp_err_t rc522_iso_dep_transceive(const uint8_t *tx, size_t tx_len,
                                   uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    if (!s_dep.active || tx == NULL || tx_len == 0U ||
        rx == NULL || rx_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t max_inf = dep_max_inf();

    uint8_t   frame[ISO_DEP_FSD];
    size_t    frame_len = 0;
    size_t    sent      = 0;
    esp_err_t err;

    /* Our half: chain the C-APDU out in pieces the card will accept. Every
     * block but the last is answered with a bare acknowledgement. */
    for (;;) {
        size_t     chunk = tx_len - sent;
        const bool more  = chunk > max_inf;
        if (more) {
            chunk = max_inf;
        }

        uint8_t out[ISO_DEP_FSD];
        out[0] = (uint8_t)(PCB_I_BLOCK | s_dep.block |
                           (more ? PCB_CHAINING : 0U));
        memcpy(&out[1], &tx[sent], chunk);

        frame_len = sizeof(frame);
        err = dep_frame(out, chunk + 1U, frame, sizeof(frame), &frame_len,
                        s_dep.fwt_ms);
        if (err != ESP_OK) {
            return err;
        }

        sent += chunk;
        if (!more) {
            break;              /* `frame` now holds the card's reply */
        }

        if ((frame[0] & PCB_R_MASK) != PCB_R_ACK ||
            (frame[0] & PCB_BLOCK_NUM) != s_dep.block) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        s_dep.block ^= PCB_BLOCK_NUM;
    }

    /* The card's half: reassemble whatever it chains back, granting waiting
     * time extensions as it asks for them. */
    size_t got = 0;
    for (;;) {
        const uint8_t pcb = frame[0];

        if ((pcb & PCB_TYPE_MASK) == PCB_TYPE_S) {
            if ((pcb & PCB_S_KIND_MASK) != PCB_S_WTX_KIND || frame_len < 2U) {
                /* An S(DESELECT) in the middle of an exchange is the card
                 * abandoning us. There is no recovery and nothing to report
                 * but that it happened. */
                return ESP_ERR_INVALID_RESPONSE;
            }

            /* S(WTX): the card is still working and wants longer. A PKOC
             * AUTHENTICATE is a P-256 signature on a smart card, so this is
             * the expected path rather than an exceptional one. The reply
             * echoes the multiplier back with the power-level bits cleared. */
            uint8_t wtxm = (uint8_t)(frame[1] & 0x3FU);
            if (wtxm == 0U || wtxm > 59U) {
                wtxm = 1U;
            }

            uint32_t grant = s_dep.fwt_ms * wtxm;
            if (grant > ISO_DEP_MAX_FWT_MS) {
                grant = ISO_DEP_MAX_FWT_MS;
            }

            const uint8_t reply[2] = { PCB_S_WTX, wtxm };
            frame_len = sizeof(frame);
            err = dep_frame(reply, sizeof(reply), frame, sizeof(frame),
                            &frame_len, grant);
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }

        /* Anything that is not an I-block here is the card asking for a
         * retransmission this driver does not implement. Reporting the
         * failure and letting the holder present the card again is simpler
         * and, for one exchange at a door, no slower. */
        if ((pcb & PCB_TYPE_MASK) != PCB_TYPE_I ||
            (pcb & PCB_BLOCK_NUM) != s_dep.block) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const size_t inf = frame_len - 1U;
        if (got + inf > rx_cap) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&rx[got], &frame[1], inf);
        got += inf;

        s_dep.block ^= PCB_BLOCK_NUM;

        if ((pcb & PCB_CHAINING) == 0U) {
            break;
        }

        /* More to come: acknowledge with the block number we just moved to,
         * which is the one the card's next block will carry. */
        const uint8_t ack = (uint8_t)(PCB_R_ACK | s_dep.block);
        frame_len = sizeof(frame);
        err = dep_frame(&ack, 1U, frame, sizeof(frame), &frame_len,
                        s_dep.fwt_ms);
        if (err != ESP_OK) {
            return err;
        }
    }

    *rx_len = got;
    return ESP_OK;
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

    /* Internal timer as the card-response timeout, started automatically at
     * the end of every transmission (TAuto). Prescaler 0x0D3E divides the
     * 13.56 MHz carrier by 2*0x0D3E+1 = 6781, giving 1999.7 Hz — a tick just
     * over 0.5 ms, which is what set_response_timeout_ms() converts against.
     *
     * 15 ms to begin with: long enough for any 14443-3 exchange, short
     * enough that polling an empty field costs almost nothing. ISO-DEP
     * raises it per exchange from the card's own ATS. */
    ESP_RETURN_ON_ERROR(reg_write(REG_T_MODE, 0x8DU), TAG, "tmode");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_PRESCALER, 0x3EU), TAG, "tprescaler");
    ESP_RETURN_ON_ERROR(set_response_timeout_ms(RC522_DEFAULT_TIMEOUT_MS),
                        TAG, "timeout");
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

    /* A card the caller never released would still be selected and would not
     * answer REQA, and the response timer could still be set to its frame
     * waiting time. Both are the caller's bug; recovering from them costs one
     * call and turns a silent stall into nothing at all. */
    rc522_release();

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

    /* Deliberately not halted here. The card stays selected and powered so
     * that a caller who wants an APDU exchange has something to talk to;
     * rc522_release() is what ends the encounter. */
    s_selected = true;

    memcpy(out->bytes, uid, uid_len);
    out->len = uid_len;
    out->sak = sak;
    return ESP_OK;
}

void rc522_release(void)
{
    if (s_dep.active) {
        /* S(DESELECT) is the protocol's own way to end the encounter, and it
         * leaves the card in the same halted state HLTA would — so there is
         * no need to follow it with one. The reply is not checked: if the
         * card has already been withdrawn there is nothing left to tell, and
         * either way we are done with it.
         *
         * The short timeout rather than the card's frame waiting time is
         * deliberate. That time exists to cover a card doing arithmetic, and
         * there is none here — but it can be five seconds, and a card taken
         * off the antenna mid-transaction would otherwise stall the polling
         * task for all of it waiting for an answer we are not going to
         * read. */
        const uint8_t req = PCB_S_DESELECT;
        uint8_t       rsp[ISO_DEP_FSD];
        size_t        len = sizeof(rsp);

        (void)dep_frame(&req, 1U, rsp, sizeof(rsp), &len,
                        RC522_DEFAULT_TIMEOUT_MS);
        s_dep.active = false;
    } else if (s_selected) {
        halt_a();
    }

    s_selected = false;

    /* Back to the short timeout, or the next poll of an empty field would
     * wait out a card's frame waiting time before deciding nothing is
     * there — seconds, several times a second. */
    (void)set_response_timeout_ms(RC522_DEFAULT_TIMEOUT_MS);
}
