#include "key_reset.h"
#include "board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "keyreset";

/* Same 50 ms window as the tamper input, and for a related but not identical
 * reason. There the concern was a stream of unsolicited status reports; here
 * it is that a bouncing contact must not silently restart a hold the person
 * pressing it believes is nearly complete. Either way, nothing is lost by
 * taking 50 ms to believe a button. */
#define STABLE_MS 50

static bool    s_down;        /* debounced; true = pressed                  */
static bool    s_candidate;   /* the reading currently being timed           */
static int64_t s_since_us;    /* when s_candidate was first seen             */
static int64_t s_down_us;     /* when the debounced press began              */
static bool    s_fired;       /* this press has already triggered a reset    */

/* One raw sample, normalised so true means pressed.
 *
 * The BOOT button shorts the pin to ground and the internal pull-up supplies
 * the other level, so pressed reads low. Unlike the tamper input there is no
 * polarity option: this is a button on the board, not a switch someone
 * chooses and wires. */
static bool read_raw(void)
{
    return gpio_get_level(BOARD_KEY_RESET) == 0;
}

esp_err_t key_reset_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_KEY_RESET,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");

    s_down      = read_raw();
    s_candidate = s_down;
    s_since_us  = esp_timer_get_time();
    s_down_us   = s_since_us;

    /* A button found already down at init is treated as an in-progress press
     * that has not been credited any time yet — s_down_us starts now. The
     * alternative, refusing to count it until released, would mean a person
     * who pressed BOOT slightly before the firmware finished booting has to
     * work out why nothing is happening. Ten seconds from here is still ten
     * seconds of deliberate holding. */
    if (s_down) {
        ESP_LOGW(TAG, "the key-reset button is already down at start-up");
    }

    ESP_LOGI(TAG, "key reset on GPIO%d, hold %u ms to return to install mode",
             BOARD_KEY_RESET, (unsigned)CONFIG_OPENREADER_KEY_RESET_HOLD_MS);
    return ESP_OK;
}

bool key_reset_poll(void)
{
    const bool    raw = read_raw();
    const int64_t now = esp_timer_get_time();

    /* Any disagreement restarts the debounce clock, so a change is believed
     * only after the line has been quiet for the full window. */
    if (raw != s_candidate) {
        s_candidate = raw;
        s_since_us  = now;
        return false;
    }

    if (raw != s_down && (now - s_since_us) >= (int64_t)STABLE_MS * 1000) {
        s_down = raw;
        if (s_down) {
            /* Time the hold from the first stable reading, not from the
             * bounce that preceded it. */
            s_down_us = s_since_us;
        } else {
            /* Released. Whatever this press earned is gone, and a new one
             * starts from zero — the hold has to be continuous. Releasing is
             * also the only thing that re-arms a press that already fired. */
            s_fired = false;
            ESP_LOGD(TAG, "key-reset button released");
        }
    }

    if (!s_down || s_fired) {
        return false;
    }
    if ((now - s_down_us) <
        (int64_t)CONFIG_OPENREADER_KEY_RESET_HOLD_MS * 1000) {
        return false;
    }

    s_fired = true;
    ESP_LOGW(TAG, "key-reset button held for the full %u ms",
             (unsigned)CONFIG_OPENREADER_KEY_RESET_HOLD_MS);
    return true;
}

uint32_t key_reset_held_ms(void)
{
    if (!s_down) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - s_down_us) / 1000);
}

uint32_t key_reset_hold_ms(void)
{
    return (uint32_t)CONFIG_OPENREADER_KEY_RESET_HOLD_MS;
}
