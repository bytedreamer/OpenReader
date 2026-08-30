#include "rs485.h"
#include "board.h"

#include "freertos/FreeRTOS.h"   /* pdMS_TO_TICKS — do not rely on uart.h
                                  * pulling this in transitively */
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "rs485";

/* One OSDP frame is at most 1440 bytes and the ACU may send back-to-back
 * commands to other devices on the bus that we filter out, so give the
 * driver room to buffer a few. */
#define RS485_RX_BUF 2048
#define RS485_TX_BUF 1024

#if CONFIG_OPENREADER_RS485_ECHO_SUPPRESS
/* Bytes we have transmitted that we expect to see arrive back on RX. See
 * the Kconfig help — only meaningful on an auto-direction transceiver that
 * leaves its receiver enabled while driving the line. */
static size_t s_echo_pending;
#endif

/* Wire-level counters. Cheap enough to keep always-on rather than behind a
 * debug flag: an installed reader that has gone quiet needs these just as
 * much as one on a bench, and two counters cost nothing. */
static uint32_t s_rx_bytes;
static uint32_t s_tx_bytes;

/* The first bytes ever seen, kept verbatim. Counted separately from
 * s_rx_bytes because it is a one-shot snapshot, not a running window —
 * what matters is what arrived when the link first came up, not the most
 * recent traffic. */
#define RS485_FIRST_CAP 16
static uint8_t s_first[RS485_FIRST_CAP];
static size_t  s_first_len;

void rs485_stats(uint32_t *rx_bytes, uint32_t *tx_bytes,
                 uint8_t *first, size_t first_cap, size_t *first_len)
{
    if (rx_bytes != NULL) { *rx_bytes = s_rx_bytes; }
    if (tx_bytes != NULL) { *tx_bytes = s_tx_bytes; }
    if (first != NULL && first_len != NULL) {
        size_t n = (s_first_len < first_cap) ? s_first_len : first_cap;
        memcpy(first, s_first, n);
        *first_len = n;
    } else if (first_len != NULL) {
        *first_len = 0;
    }
}

esp_err_t rs485_init(int baud)
{
    const uart_config_t cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        /* The MAX13487 needs no driver-enable line, so there is no hardware
         * flow control and no RTS pin to hand the driver. */
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BOARD_RS485_UART, RS485_RX_BUF,
                                        RS485_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(BOARD_RS485_UART, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(BOARD_RS485_UART, BOARD_RS485_TX, BOARD_RS485_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "UART%d up at %d baud (TX=%d RX=%d)",
             BOARD_RS485_UART, baud, BOARD_RS485_TX, BOARD_RS485_RX);
    return ESP_OK;
}

/* Non-blocking: return whatever has already arrived, never wait for more.
 * osdp_pd_tick() calls this every pass and reassembles frames across calls,
 * so a partial read is normal and costs nothing. */
static int rs485_read(void *user, uint8_t *buf, size_t cap)
{
    (void)user;

    int n = uart_read_bytes(BOARD_RS485_UART, buf, cap, 0);
    if (n <= 0) {
        return 0;
    }

    /* Counted before echo suppression, deliberately: this is the wire-level
     * question of whether anything at all is arriving on the pin, which is
     * exactly what you need when nothing is working. */
    s_rx_bytes += (uint32_t)n;
    if (s_first_len < RS485_FIRST_CAP) {
        size_t room = RS485_FIRST_CAP - s_first_len;
        size_t take = ((size_t)n < room) ? (size_t)n : room;
        memcpy(&s_first[s_first_len], buf, take);
        s_first_len += take;
    }

#if CONFIG_OPENREADER_RS485_ECHO_SUPPRESS
    if (s_echo_pending > 0) {
        size_t drop = (s_echo_pending < (size_t)n) ? s_echo_pending
                                                   : (size_t)n;
        s_echo_pending -= drop;
        n -= (int)drop;
        if (n > 0) {
            /* Real bytes followed our echo in the same read; slide them
             * down so the caller sees only those. */
            memmove(buf, buf + drop, (size_t)n);
        }
    }
#endif

    return n;
}

/* The PD hands us a whole reply in one call, which is what a half-duplex
 * bus wants. We block until the last bit has actually left the shift
 * register: on a transceiver without a DE pin that is what defines the end
 * of our turn on the wire, and returning early would let the next thing we
 * do race the tail of the frame. */
static int rs485_write(void *user, const uint8_t *buf, size_t len)
{
    (void)user;

    int written = uart_write_bytes(BOARD_RS485_UART, buf, len);
    if (written < 0) {
        return 0;
    }
    s_tx_bytes += (uint32_t)written;

    /* 100 ms is far longer than any legal frame takes to clock out, even at
     * 9600 baud (1440 bytes ~= 1.5 s would exceed it, but an OSDP reply from
     * this PD is tens of bytes). A timeout here means the UART is wedged. */
    if (uart_wait_tx_done(BOARD_RS485_UART, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "TX did not drain");
    }

#if CONFIG_OPENREADER_RS485_ECHO_SUPPRESS
    s_echo_pending += (size_t)written;
#endif

    /* A short write is treated by the PD as a transmission error and the
     * reply is dropped, which is the correct outcome — a half-sent frame
     * would just fail the ACU's CRC check. */
    return written;
}

void rs485_emit_marker(void)
{
    /* 0xA5/0x5A alternate every other bit, so they exercise the line in both
     * directions and are unmistakable in a monitor's hex dump. Not a legal
     * OSDP frame, so nothing on the bus will act on it. */
    static const uint8_t marker[] = { 0xA5, 0x5A, 0xA5, 0x5A };
    int n = uart_write_bytes(BOARD_RS485_UART, marker, sizeof(marker));
    if (n > 0) {
        s_tx_bytes += (uint32_t)n;
    }
    (void)uart_wait_tx_done(BOARD_RS485_UART, pdMS_TO_TICKS(100));
#if CONFIG_OPENREADER_RS485_ECHO_SUPPRESS
    if (n > 0) { s_echo_pending += (size_t)n; }
#endif
}

static uint32_t rs485_now_ms(void *user)
{
    (void)user;
    /* esp_timer is a 64-bit monotonic microsecond counter that survives
     * light sleep. Truncating to 32 bits is fine: the PD does all its time
     * comparisons with unsigned subtraction, so the ~49.7-day wrap is
     * handled. */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const osdp_pd_transport_t *rs485_transport(void)
{
    static const osdp_pd_transport_t transport = {
        .read   = rs485_read,
        .write  = rs485_write,
        .now_ms = rs485_now_ms,
        .user   = NULL,
    };
    return &transport;
}
