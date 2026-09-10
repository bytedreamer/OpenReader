#include "tamper.h"
#include "board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "tamper";

/* How long a new reading has to hold before it counts.
 *
 * Generous by switch-bounce standards, where single-figure milliseconds
 * would settle any contact. The reason is not the switch but what happens
 * downstream: every accepted change sends the ACU an unsolicited status
 * report, so a marginal switch or a door that shakes in its frame would
 * become a stream of them. Nothing is lost by taking 50 ms to believe it —
 * there is no such thing as a tamper that must be reported faster. */
#define TAMPER_STABLE_MS 50

static bool    s_state;      /* debounced; true = tampered */
static bool    s_candidate;  /* the reading currently being timed */
static int64_t s_since_us;

/* One raw sample, normalised so true always means tampered.
 *
 * The wiring this expects is a normally-open switch to ground working
 * against the internal pull-up: the closed enclosure holds the contact open
 * and the pull-up holds the pin high, and opening the box closes the contact
 * and pulls the pin low. So low means tampered.
 *
 * The cost of that polarity is that it does not supervise its own wiring — a
 * cut, lifted or never-connected wire reads exactly like an intact quiet
 * loop. CONFIG_OPENREADER_TAMPER_NORMALLY_CLOSED inverts it for a
 * normally-closed switch, which does have that property. */
static bool read_raw(void)
{
    const int level = gpio_get_level(BOARD_TAMPER);
#if CONFIG_OPENREADER_TAMPER_NORMALLY_CLOSED
    return level != 0;
#else
    return level == 0;
#endif
}

esp_err_t tamper_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_TAMPER,
        .mode         = GPIO_MODE_INPUT,
        /* The switch only ever drives one way. The pull-up supplies the
         * other level, and is what keeps the pin at a definite level while
         * the switch is open instead of floating between the two. */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");

    /* Adopt the switch's current reading rather than assuming normal. A
     * reader powered up inside an already-open enclosure should say so on
     * the first osdp_LSTAT, not wait for someone to close it and open it
     * again before it will admit anything is wrong. */
    s_state     = read_raw();
    s_candidate = s_state;
    s_since_us  = esp_timer_get_time();

    ESP_LOGI(TAG, "tamper input on GPIO%d, currently %s",
             BOARD_TAMPER, s_state ? "TAMPER" : "normal");
    return ESP_OK;
}

bool tamper_poll(void)
{
    const bool    raw = read_raw();
    const int64_t now = esp_timer_get_time();

    /* Any disagreement restarts the clock, so the filter reports a change
     * only after the line has been quiet for the full window — bouncing
     * contacts never accumulate credit towards one. */
    if (raw != s_candidate) {
        s_candidate = raw;
        s_since_us  = now;
        return false;
    }
    if (raw == s_state) {
        return false;
    }
    if ((now - s_since_us) < (int64_t)TAMPER_STABLE_MS * 1000) {
        return false;
    }

    s_state = raw;
    ESP_LOGW(TAG, "enclosure %s",
             s_state ? "TAMPERED" : "restored to normal");
    return true;
}

bool tamper_active(void)
{
    return s_state;
}
