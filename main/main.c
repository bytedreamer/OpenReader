/* OpenReader — an OSDP v2.2.2 card reader on the Waveshare
 * ESP32-C6-LCD-1.47, built on Z-bit Systems' OSDP-Embedded PD library.
 *
 * Two tasks:
 *   - the OSDP task services the RS-485 bus and owns every osdp_pd_* call
 *   - the card task polls the RC522 and hands UIDs over a queue
 *
 * See docs/HARDWARE.md to build one.
 */
#include "board.h"
#include "rs485.h"
#include "rc522.h"
#include "display.h"
#include "buzzer.h"
#include "tamper.h"
#include "osdp_reader.h"
#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"            /* esp_err_to_name */
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "main";

/* How often to look for a card. 100 ms is comfortably faster than anyone
 * can present and withdraw a badge, and each poll of an empty field costs
 * only the RC522's own ~25 ms response timer at worst. */
#define CARD_POLL_INTERVAL_MS 100

#if defined(BOARD_SPI_HOST)
/* Bring up hardware SPI2 for whichever peripheral owns it — the LCD when
 * the display is built, otherwise an RC522 set to the hardware bus. board.h
 * decides and defines BOARD_SPI_HOST only in those cases; this follows it.
 *
 * A build with the display off and the RC522 bit-banged claims no hardware
 * SPI at all, and then this does not exist. */
static esp_err_t spi_bus_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num     = BOARD_SPI_SCLK,
        .mosi_io_num     = BOARD_SPI_MOSI,
        .miso_io_num     = BOARD_SPI_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
#if CONFIG_OPENREADER_DISPLAY
        /* A whole 172x320 frame at 16 bpp in one transfer. The display
         * pushes partial bands most of the time, but the first paint after
         * any change is the full screen. */
        .max_transfer_sz = 172 * 320 * 2,
#else
        /* The largest single transfer the RC522 driver makes is a handful
         * of bytes. */
        .max_transfer_sz = 64,
#endif
    };
    return spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
}
#endif /* BOARD_SPI_HOST */

#if CONFIG_OPENREADER_RC522
static void card_task(void *arg)
{
    (void)arg;

    rc522_uid_t last;
    memset(&last, 0, sizeof(last));
    uint32_t last_ms = 0;

    for (;;) {
        rc522_uid_t uid;
        esp_err_t   err = rc522_poll(&uid);

        if (err == ESP_OK) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

            /* Suppress the same card re-read while it sits on the antenna.
             * Comparing the UID as well as the time matters: presenting a
             * different card immediately after one should report at once,
             * not wait out the previous card's window. */
            bool same = (uid.len == last.len)
                        && (memcmp(uid.bytes, last.bytes, uid.len) == 0);
            bool fresh = !same
                         || (now - last_ms)
                            >= (uint32_t)CONFIG_OPENREADER_CARD_REPEAT_MS;

            if (fresh) {
                /* Three chars per byte ("XX:"), with the final byte's
                 * separator omitted leaving room for the terminator. */
                char hex[RC522_UID_MAX_BYTES * 3];
                for (uint8_t i = 0; i < uid.len; i++) {
                    snprintf(&hex[i * 3], sizeof(hex) - (size_t)i * 3,
                             "%02X%s", uid.bytes[i],
                             (i + 1 < uid.len) ? ":" : "");
                }
                ESP_LOGI(TAG, "card %s (SAK 0x%02X)", hex, uid.sak);

#if CONFIG_OPENREADER_DISPLAY
                /* Straight to the panel, not via the OSDP task. What the
                 * reader face shows is what this reader physically read;
                 * whether the ACU ever collects it is a separate question,
                 * and conflating the two would make a full event queue look
                 * like a card that never scanned. */
                display_set_card(uid.bytes, uid.len);
#endif

                if (osdp_reader_submit_card(&uid) != ESP_OK) {
                    ESP_LOGW(TAG, "could not hand card to the OSDP task");
                }
                last    = uid;
                last_ms = now;
            }
        } else if (err != ESP_ERR_NOT_FOUND) {
            /* NOT_FOUND is an empty field, which is the steady state and
             * says nothing. Anything else is a real read failure — usually
             * a card pulled away mid-anticollision, occasionally wiring. */
            ESP_LOGD(TAG, "read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(CARD_POLL_INTERVAL_MS));
    }
}
#endif /* CONFIG_OPENREADER_RC522 */

void app_main(void)
{
    ESP_LOGI(TAG, "OpenReader starting");

    ESP_ERROR_CHECK(status_led_init());
#if CONFIG_OPENREADER_BUZZER
    /* Before anything else can command it, and before the OSDP task starts:
     * buzzer_init leaves the pin at its silent level, which on an
     * active-low part is not the state gpio_config would have left it in. */
    ESP_ERROR_CHECK(buzzer_init());
#endif
#if CONFIG_OPENREADER_TAMPER
    /* Before osdp_reader_init(), so the very first osdp_LSTAT the ACU sends
     * is answered from the switch rather than from the default. */
    ESP_ERROR_CHECK(tamper_init());
#endif
#if defined(BOARD_SPI_HOST)
    ESP_ERROR_CHECK(spi_bus_init());
#endif
    ESP_ERROR_CHECK(rs485_init(CONFIG_OPENREADER_RS485_BAUD));

    /* A failed RC522 is not fatal. The PD should still come up and answer
     * the ACU — a reader that reports itself online but reads no cards is
     * diagnosable from the head end, whereas one that never boots looks
     * identical to a dead cable. */
    bool card_reader_ok = false;
#if CONFIG_OPENREADER_RC522
    esp_err_t err = rc522_init();
    card_reader_ok = (err == ESP_OK);
    if (!card_reader_ok) {
        ESP_LOGE(TAG, "RC522 init failed (%s) — continuing without a card "
                      "reader so the OSDP link still comes up",
                 esp_err_to_name(err));
    }
#endif

#if CONFIG_OPENREADER_DISPLAY
    /* Same reasoning as the RC522 above: a panel that does not come up is
     * a missing convenience, not a reason to leave the door without a PD. */
    esp_err_t disp_err = display_init();
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed (%s) — continuing headless",
                 esp_err_to_name(disp_err));
    } else {
        display_set_reader_present(card_reader_ok);
    }
#endif

    ESP_ERROR_CHECK(osdp_reader_init());

#if CONFIG_OPENREADER_RC522
    if (card_reader_ok) {
        /* Priority 5 against the OSDP task's 10: servicing the bus always
         * wins. A card read that waits an extra millisecond is invisible; a
         * poll answered late is a comms failure at the ACU. */
        xTaskCreate(card_task, "card", 4096, NULL, 5, NULL);
    }
#endif

    /* app_main's own task becomes the OSDP task rather than spawning a
     * third one. Raise its priority first — app_main starts at 1, and the
     * bus deserves better than that. */
    vTaskPrioritySet(NULL, 10);
    osdp_reader_run();
}
