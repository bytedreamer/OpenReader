#include "status_led.h"
#include "board.h"

#include "led_strip.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "osdp/osdp_commands.h"   /* osdp_led_color_t */

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static bool     s_online;
static uint8_t  s_osdp_color = OSDP_LED_BLACK;

/* Full scale, because this build is a demo that has to read across a room.
 *
 * A reader mounted at a door wants this lower — the WS2812 is genuinely
 * unpleasant at full brightness up close, and a reader LED is a status
 * indicator rather than illumination; 40 was that judgement. Every colour
 * below is mixed proportionally to this, so amber stays amber and the
 * offline breath keeps its ramp wherever it is set. */
#define LED_LEVEL 255U

/* The one place a colour reaches the hardware — and the one place the
 * board's byte order is corrected, by handing the driver red and green
 * swapped.
 *
 * Measured, not assumed. With the strip declared GRB, an ACU commanding
 * green (osdp_LED colour 2, resolved here to r=0 g=40 b=0) lit the pixel
 * red: GRB puts the green argument out as the first byte on the wire, and
 * this part reads its first byte as red. So the pixel is RGB-ordered while
 * the driver can only speak GRB — led_strip 2.5.5 offers GRB and GRBW and
 * no way to say otherwise. Swapping the two arguments here puts the right
 * byte in the right slot and costs nothing.
 *
 * What this bug looks like if it ever comes back on another board
 * revision: red and green trade places, and so do cyan and magenta, while
 * blue, white and off stay correct — which is exactly why a quick sweep
 * through the colours can look like it passed. */
static void paint(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip == NULL) {
        return;
    }
    (void)led_strip_set_pixel(s_strip, 0, g, r, b);
    (void)led_strip_refresh(s_strip);
}

esp_err_t status_led_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num   = BOARD_RGB_LED,
        .max_leds         = 1,
        /* GRB is the only non-white order this component offers (2.5.5
         * has GRB and GRBW and nothing else), and it does not match this
         * board's pixel — paint() below corrects for that. Field names
         * here are the led_strip 2.x API, which is what the caret pin in
         * idf_component.yml resolves to; the 3.x line renamed
         * led_pixel_format to color_component_format, and if you ever
         * widen that pin this struct is the thing that breaks. */
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
    };
    /* RMT rather than SPI: the SPI2 bus is already carrying the LCD, the SD
     * slot and the RC522, and the WS2812 has no reason to contend with
     * them. */
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .flags.with_dma    = false,
    };

    ESP_RETURN_ON_ERROR(
        led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip),
        TAG, "led_strip");

    (void)led_strip_clear(s_strip);
    ESP_LOGI(TAG, "WS2812 on GPIO%d", BOARD_RGB_LED);
    return ESP_OK;
}

/* Spec Table 18 colours onto RGB. Amber is the one that needs a real
 * choice: an even red/green mix reads green on a WS2812, so red is left at
 * full and green cut to roughly a third. */
static void osdp_color_to_rgb(uint8_t color, uint8_t *r, uint8_t *g,
                              uint8_t *b)
{
    switch (color) {
    case OSDP_LED_RED:     *r = LED_LEVEL; *g = 0;             *b = 0;         break;
    case OSDP_LED_GREEN:   *r = 0;         *g = LED_LEVEL;     *b = 0;         break;
    case OSDP_LED_AMBER:   *r = LED_LEVEL; *g = LED_LEVEL / 3; *b = 0;         break;
    case OSDP_LED_BLUE:    *r = 0;         *g = 0;             *b = LED_LEVEL; break;
    case OSDP_LED_MAGENTA: *r = LED_LEVEL; *g = 0;             *b = LED_LEVEL; break;
    case OSDP_LED_CYAN:    *r = 0;         *g = LED_LEVEL;     *b = LED_LEVEL; break;
    case OSDP_LED_WHITE:   *r = LED_LEVEL; *g = LED_LEVEL;     *b = LED_LEVEL; break;
    case OSDP_LED_BLACK:
    default:               *r = 0;         *g = 0;             *b = 0;         break;
    }
}

void status_led_set_osdp(uint8_t osdp_color)
{
    s_osdp_color = osdp_color;
    if (!s_online) {
        return;   /* the offline animation owns the LED */
    }
    uint8_t r, g, b;
    osdp_color_to_rgb(osdp_color, &r, &g, &b);
    paint(r, g, b);
}

void status_led_set_link(bool online)
{
    if (online == s_online) {
        return;
    }
    s_online = online;
    if (online) {
        /* Hand the LED straight back to whatever the ACU last asked for,
         * rather than blanking and waiting for the next osdp_LED. */
        status_led_set_osdp(s_osdp_color);
    }
}

void status_led_tick(void)
{
    if (s_online) {
        return;
    }
    /* A ~2 s triangle breath in blue. Distinct at a glance from any colour
     * an ACU would command, which is the point: "nobody is polling me" has
     * to be visibly different from "the ACU is telling me to show blue". */
    uint32_t ms    = (uint32_t)(esp_timer_get_time() / 1000) % 2000U;
    uint32_t phase = (ms < 1000U) ? ms : (2000U - ms);
    uint8_t  level = (uint8_t)((phase * LED_LEVEL) / 1000U);
    paint(0, 0, level);
}
