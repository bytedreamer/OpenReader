#include "buzzer.h"
#include "board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "buzzer";

static bool s_online;
static bool s_sounding;   /* what the ACU last commanded          */
static bool s_local;      /* a local operation holding it on      */

/* Cheap three-pin modules disagree about polarity: some sound when the input
 * is driven high, others when it is pulled low (the transistor sits on the
 * other side of the coil). Getting it backwards is not subtle — the reader
 * screams continuously from boot and falls silent only when commanded — but
 * it is a confusing five minutes, so it is one Kconfig switch rather than a
 * soldering-iron problem. */
#if CONFIG_OPENREADER_BUZZER_ACTIVE_LOW
#define LEVEL_SOUNDING 0
#define LEVEL_SILENT   1
#else
#define LEVEL_SOUNDING 1
#define LEVEL_SILENT   0
#endif

static void apply(void)
{
    /* The local hold ignores the link. See buzzer_local(): a reader being
     * re-keyed is very often one that is not being polled, and that is
     * precisely when the confirmation beep has to happen. */
    const bool sound = s_local || (s_online && s_sounding);
    gpio_set_level(BOARD_BUZZER, sound ? LEVEL_SOUNDING : LEVEL_SILENT);
}

esp_err_t buzzer_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BUZZER,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");

    /* Silent before anything else runs. gpio_config leaves an output at 0,
     * which on an active-low part is "sounding" — so this is not a
     * belt-and-braces line, it is the difference between a quiet boot and a
     * reader that shrieks until the first osdp_BUZ arrives. */
    s_online   = false;
    s_sounding = false;
    s_local    = false;
    apply();

    ESP_LOGI(TAG, "active sounder on GPIO%d (%s)", BOARD_BUZZER,
             LEVEL_SOUNDING ? "active high" : "active low");
    return ESP_OK;
}

void buzzer_set_osdp(bool sounding)
{
    s_sounding = sounding;
    apply();
}

void buzzer_set_link(bool online)
{
    s_online = online;
    apply();
}

void buzzer_local(bool sounding)
{
    s_local = sounding;
    apply();
}
