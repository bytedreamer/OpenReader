/* OpenReader — an OSDP v2.2.2 card reader on the Waveshare
 * ESP32-C6-LCD-1.47, built on Z-bit Systems' OSDP-Embedded PD library.
 *
 * Two tasks:
 *   - the OSDP task services the RS-485 bus and owns every osdp_pd_* call
 *   - the card task polls the RC522 and hands credentials over a queue
 *
 * See docs/HARDWARE.md to build one.
 */
#include "board.h"
#include "rs485.h"
#include "rc522.h"
#include "credential.h"
#include "pkoc.h"
#include "display.h"
#include "buzzer.h"
#include "tamper.h"
#include "osdp_reader.h"
#include "status_led.h"
#include "sc_key.h"
#include "key_reset.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"            /* esp_err_to_name */
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"          /* the Secure Channel key store lives here */
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
/* What did this card actually give us?
 *
 * PKOC first whenever the card's SAK says it speaks ISO/IEC 14443-4, because
 * a card that can prove who it is should be asked to rather than have its
 * UID copied off it. The three outcomes are deliberately not symmetrical:
 *
 *   a verified credential   report it
 *   no applet, or the
 *   exchange broke down     report the UID, if this build allows it
 *   signature did not
 *   verify                  report nothing, ever
 *
 * That last row is the one that matters. A card that cannot prove it holds
 * the private key has told us nothing at all, and falling back to the UID it
 * also happens to carry would hand the ACU something to make a decision on
 * — quietly turning a failed proof into a successful read.
 */
static bool read_credential(const rc522_uid_t *uid, credential_t *out)
{
#if CONFIG_OPENREADER_PKOC
    if (rc522_uid_is_iso14443_4(uid)) {
        const esp_err_t err = pkoc_read(out);

        if (err == ESP_OK) {
            return true;
        }
        if (err == PKOC_ERR_BAD_SIGNATURE) {
            ESP_LOGW(TAG, "card rejected: it offered a public key it could "
                          "not prove it owns");
            return false;
        }
        /* No applet, or an exchange that fell over — usually a card taken
         * off the antenna partway through, since a PKOC transaction is
         * several round trips and a signature long. Either way there is
         * still a UID underneath. */
    }

#if !CONFIG_OPENREADER_PKOC_UID_FALLBACK
    /* A PKOC-only door. A UID is not evidence of anything, and a panel that
     * accepts both has the weaker of the two as its real security. */
    ESP_LOGD(TAG, "no PKOC credential and the UID fallback is off");
    return false;
#endif
#endif /* CONFIG_OPENREADER_PKOC */

    memset(out, 0, sizeof(*out));
    out->kind      = CREDENTIAL_UID;
    out->len       = uid->len;
    out->bit_count = (uint16_t)(uid->len * 8U);
    memcpy(out->bytes, uid->bytes, uid->len);
    return true;
}

static void card_task(void *arg)
{
    (void)arg;

    credential_t last;
    memset(&last, 0, sizeof(last));
    uint32_t last_ms = 0;

    for (;;) {
        rc522_uid_t uid;
        esp_err_t   err = rc522_poll(&uid);

        if (err == ESP_OK) {
            credential_t cred;
            const bool   have = read_credential(&uid, &cred);

            /* The card is left selected by rc522_poll() so that the PKOC
             * exchange above has something to talk to. Letting it go is
             * ours to do, and it has to happen whether or not we got a
             * credential out of it. */
            rc522_release();

            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

            /* Suppress the same card re-read while it sits on the antenna.
             * Comparing the credential as well as the time matters:
             * presenting a different card immediately after one should
             * report at once, not wait out the previous card's window. */
            bool fresh = have
                         && (!credential_equal(&cred, &last)
                             || (now - last_ms)
                                >= (uint32_t)CONFIG_OPENREADER_CARD_REPEAT_MS);

            if (fresh) {
                char hex[CREDENTIAL_MAX_BYTES * 2 + 1];
                (void)credential_hex(&cred, hex, sizeof(hex));

                if (cred.kind == CREDENTIAL_PKOC) {
                    /* The credential itself stays out of the ordinary log.
                     * A PKOC public key is not a secret — that is the whole
                     * point of it — but it is still the value that names a
                     * person, and a console printing it on every read is a
                     * transcript of who went through this door. What the log
                     * needs is that a card authenticated and how wide the
                     * credential was; the bits themselves are a bench
                     * question, so they move to debug. */
                    ESP_LOGI(TAG, "card PKOC %s (%u bits)",
                             cred.verified ? "verified" : "NOT VERIFIED",
                             (unsigned)cred.bit_count);
                    ESP_LOGD(TAG, "PKOC credential %s", hex);
                } else {
                    ESP_LOGI(TAG, "card UID %s (%u bits, SAK 0x%02X)", hex,
                             (unsigned)cred.bit_count, uid.sak);
                }

#if CONFIG_OPENREADER_DISPLAY
                /* Straight to the panel, not via the OSDP task. What the
                 * reader face shows is what this reader physically read;
                 * whether the ACU ever collects it is a separate question,
                 * and conflating the two would make a full event queue look
                 * like a card that never scanned. */
                display_set_card(&cred);
#endif

                if (osdp_reader_submit_card(&cred) != ESP_OK) {
                    ESP_LOGW(TAG, "could not hand card to the OSDP task");
                }
                last    = cred;
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

#if CONFIG_OPENREADER_SECURE_CHANNEL
/* Bring up the flash key/value store that holds the Secure Channel key.
 *
 * The recovery case is the interesting one. A partition that is full, or
 * written by a version of NVS this build cannot read, is repaired by erasing
 * it — which is a reasonable thing to do to a store whose only contents are
 * a key that can be re-issued by the panel, and an unreasonable thing to do
 * silently. Erasing it drops the reader back to install mode, so it says so
 * at ESP_LOGE: someone will otherwise spend an afternoon wondering why a
 * reader they keyed last week is asking for SCBK-D again.
 *
 * A failure that erasing does not fix is not fatal either. The reader still
 * comes up, sc_key_load() reports the store as unreadable, and the PD
 * refuses every handshake rather than falling back to a published key — see
 * bind_sc(). A reader that will not talk is diagnosable from the head end; a
 * reader that will not boot looks exactly like a dead cable. */
static void key_store_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "the NVS partition is unusable (%s) and is being "
                      "erased — ANY STORED SECURE CHANNEL KEY IS GONE and "
                      "this reader will come up in install mode",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s) — the Secure Channel key cannot "
                      "be read and every handshake will be refused",
                 esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(sc_key_init());
}
#endif /* CONFIG_OPENREADER_SECURE_CHANNEL */

void app_main(void)
{
    ESP_LOGI(TAG, "OpenReader starting");

#if CONFIG_OPENREADER_SECURE_CHANNEL
    /* First, before any peripheral. osdp_reader_init() reads the store to
     * decide which key this PD comes up on, and everything between here and
     * there is hardware that has no opinion about it. */
    key_store_init();
#endif
#if CONFIG_OPENREADER_KEY_RESET
    /* Also early, and for a reason worth stating: someone who has already
     * decided to reset the key may well be holding the button as the board
     * powers up. Configuring the pin now means the hold starts being counted
     * from roughly when they pressed it rather than from whenever the last
     * SPI peripheral finished probing. */
    ESP_ERROR_CHECK(key_reset_init());
#endif

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
#if CONFIG_OPENREADER_PKOC
    /* Same reasoning again: a reader that cannot do PKOC should still read
     * the badges it can rather than refuse to boot. pkoc_init() binds the
     * crypto and turns on an entropy source; there is nothing in it that a
     * working board fails. */
    if (pkoc_init() != ESP_OK) {
        ESP_LOGE(TAG, "PKOC init failed — continuing with UIDs only");
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
         * poll answered late is a comms failure at the ACU. That ordering is
         * what makes it safe to verify a P-256 signature on this task — tens
         * of milliseconds of arithmetic that the OSDP task simply preempts.
         *
         * The stack is doubled for PKOC because mbedTLS's elliptic curve
         * code is where it goes: a UID-only reader never needs more than the
         * 4 KB this had before. */
#if CONFIG_OPENREADER_PKOC
        const uint32_t card_stack = 8192;
#else
        const uint32_t card_stack = 4096;
#endif
        xTaskCreate(card_task, "card", card_stack, NULL, 5, NULL);
    }
#endif

    /* app_main's own task becomes the OSDP task rather than spawning a
     * third one. Raise its priority first — app_main starts at 1, and the
     * bus deserves better than that. */
    vTaskPrioritySet(NULL, 10);
    osdp_reader_run();
}
