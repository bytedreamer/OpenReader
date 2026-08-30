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

/* Kept dim on purpose. The WS2812 on this board is bright enough at full
 * scale to be unpleasant at a door, and a reader LED is a status indicator,
 * not illumination. */
#define LED_LEVEL 40U

/* Last colour handed to the strip, kept so the LCD can mirror it. Written
 * only by paint(); each field is a byte, so a reader on another task sees
 * one whole component or the other, never a torn one. A frame rendered
 * across an update shows the previous colour for one more frame, which at
 * 20 fps nobody can see. */
static volatile uint8_t s_cur_r, s_cur_g, s_cur_b;

static void paint(uint8_t r, uint8_t g, uint8_t b)
{
    s_cur_r = r;
    s_cur_g = g;
    s_cur_b = b;
    if (s_strip == NULL) {
        return;
    }
    (void)led_strip_set_pixel(s_strip, 0, r, g, b);
    (void)led_strip_refresh(s_strip);
}

void status_led_current_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Scale LED_LEVEL up to 255 rather than normalising each frame to its
     * own brightest channel: normalising would flatten the offline breath
     * into a constant blue and turn amber into something close to yellow. */
    *r = (uint8_t)((s_cur_r * 255U) / LED_LEVEL);
    *g = (uint8_t)((s_cur_g * 255U) / LED_LEVEL);
    *b = (uint8_t)((s_cur_b * 255U) / LED_LEVEL);
}

esp_err_t status_led_init(void)
{
    /* Field names here are the led_strip 2.x API, which is what the caret
     * pin in idf_component.yml resolves to. The 3.x line renamed
     * led_pixel_format to color_component_format — if you ever widen that
     * pin, this struct is the thing that breaks. */
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num   = BOARD_RGB_LED,
        .max_leds         = 1,
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
