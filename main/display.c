#include "display.h"
#include "board.h"
#include "status_led.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "osdp/osdp_commands.h"   /* osdp_led_color_t */

#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

/* The panel is 172x320 inside a controller that addresses 240x320, so the
 * visible columns start 34 in. Getting this wrong does not fail loudly — it
 * shifts everything sideways and wraps, which reads as a corrupt frame
 * rather than as an offset. */
#define LCD_W        172
#define LCD_H        320
#define LCD_X_GAP    34
#define LCD_Y_GAP    0

/* ---- Colour -------------------------------------------------------------
 *
 * RGB565, byte-swapped where it is built. The ST7789 wants the high byte
 * first and this core is little-endian, so a plain uint16_t arrives with
 * its bytes reversed — which is a confusing thing to debug on a board whose
 * whole job is showing a colour. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | \
                                    (((g) & 0xFC) << 3) | \
                                    ((b) >> 3)))
#define RGB(r, g, b)    ((uint16_t)((RGB565(r, g, b) >> 8) | \
                                    (RGB565(r, g, b) << 8)))

#define C_BG       RGB(0x0B, 0x0E, 0x14)
#define C_PANEL    RGB(0x15, 0x1A, 0x24)
#define C_LINE     RGB(0x23, 0x2A, 0x38)
#define C_OFFDISC  RGB(0x1A, 0x20, 0x30)
#define C_TEXT     RGB(0xE6, 0xEA, 0xF2)
#define C_MUTED    RGB(0x78, 0x84, 0x9A)
#define C_ACCENT   RGB(0x4C, 0x8D, 0xFF)
#define C_OK       RGB(0x3D, 0xDC, 0x84)
#define C_BAD      RGB(0xFF, 0x5C, 0x5C)

/* ---- 5x7 font, column-major, ASCII 0x20..0x5A --------------------------
 *
 * Uppercase only, and everything drawn through here is upper-cased on the
 * way in. A reader face carries a handful of short labels; a lowercase half
 * we would never use costs more than it earns. */
#define FONT_FIRST 0x20
#define FONT_LAST  0x5A
#define GLYPH_W    5
#define GLYPH_H    7

static const uint8_t s_font[FONT_LAST - FONT_FIRST + 1][GLYPH_W] = {
    {0x00,0x00,0x00,0x00,0x00},  /* space */
    {0x00,0x00,0x5F,0x00,0x00},  /* !  */
    {0x00,0x07,0x00,0x07,0x00},  /* "  */
    {0x14,0x7F,0x14,0x7F,0x14},  /* #  */
    {0x24,0x2A,0x7F,0x2A,0x12},  /* $  */
    {0x23,0x13,0x08,0x64,0x62},  /* %  */
    {0x36,0x49,0x55,0x22,0x50},  /* &  */
    {0x00,0x05,0x03,0x00,0x00},  /* apostrophe */
    {0x00,0x1C,0x22,0x41,0x00},  /* (  */
    {0x00,0x41,0x22,0x1C,0x00},  /* )  */
    {0x14,0x08,0x3E,0x08,0x14},  /* *  */
    {0x08,0x08,0x3E,0x08,0x08},  /* +  */
    {0x00,0x50,0x30,0x00,0x00},  /* ,  */
    {0x08,0x08,0x08,0x08,0x08},  /* -  */
    {0x00,0x60,0x60,0x00,0x00},  /* .  */
    {0x20,0x10,0x08,0x04,0x02},  /* /  */
    {0x3E,0x51,0x49,0x45,0x3E},  /* 0  */
    {0x00,0x42,0x7F,0x40,0x00},  /* 1  */
    {0x42,0x61,0x51,0x49,0x46},  /* 2  */
    {0x21,0x41,0x45,0x4B,0x31},  /* 3  */
    {0x18,0x14,0x12,0x7F,0x10},  /* 4  */
    {0x27,0x45,0x45,0x45,0x39},  /* 5  */
    {0x3C,0x4A,0x49,0x49,0x30},  /* 6  */
    {0x01,0x71,0x09,0x05,0x03},  /* 7  */
    {0x36,0x49,0x49,0x49,0x36},  /* 8  */
    {0x06,0x49,0x49,0x29,0x1E},  /* 9  */
    {0x00,0x36,0x36,0x00,0x00},  /* :  */
    {0x00,0x56,0x36,0x00,0x00},  /* ;  */
    {0x08,0x14,0x22,0x41,0x00},  /* <  */
    {0x14,0x14,0x14,0x14,0x14},  /* =  */
    {0x00,0x41,0x22,0x14,0x08},  /* >  */
    {0x02,0x01,0x51,0x09,0x06},  /* ?  */
    {0x32,0x49,0x79,0x41,0x3E},  /* @  */
    {0x7E,0x11,0x11,0x11,0x7E},  /* A  */
    {0x7F,0x49,0x49,0x49,0x36},  /* B  */
    {0x3E,0x41,0x41,0x41,0x22},  /* C  */
    {0x7F,0x41,0x41,0x22,0x1C},  /* D  */
    {0x7F,0x49,0x49,0x49,0x41},  /* E  */
    {0x7F,0x09,0x09,0x09,0x01},  /* F  */
    {0x3E,0x41,0x49,0x49,0x7A},  /* G  */
    {0x7F,0x08,0x08,0x08,0x7F},  /* H  */
    {0x00,0x41,0x7F,0x41,0x00},  /* I  */
    {0x20,0x40,0x41,0x3F,0x01},  /* J  */
    {0x7F,0x08,0x14,0x22,0x41},  /* K  */
    {0x7F,0x40,0x40,0x40,0x40},  /* L  */
    {0x7F,0x02,0x0C,0x02,0x7F},  /* M  */
    {0x7F,0x04,0x08,0x10,0x7F},  /* N  */
    {0x3E,0x41,0x41,0x41,0x3E},  /* O  */
    {0x7F,0x09,0x09,0x09,0x06},  /* P  */
    {0x3E,0x41,0x51,0x21,0x5E},  /* Q  */
    {0x7F,0x09,0x19,0x29,0x46},  /* R  */
    {0x46,0x49,0x49,0x49,0x31},  /* S  */
    {0x01,0x01,0x7F,0x01,0x01},  /* T  */
    {0x3F,0x40,0x40,0x40,0x3F},  /* U  */
    {0x1F,0x20,0x40,0x20,0x1F},  /* V  */
    {0x3F,0x40,0x38,0x40,0x3F},  /* W  */
    {0x63,0x14,0x08,0x14,0x63},  /* X  */
    {0x07,0x08,0x70,0x08,0x07},  /* Y  */
    {0x61,0x51,0x49,0x45,0x43},  /* Z  */
};

static esp_lcd_panel_handle_t s_panel;
static uint16_t              *s_fb;

/* ---- Primitives --------------------------------------------------------- */

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) { w = LCD_W - x; }
    if (y + h > LCD_H) { h = LCD_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; row++) {
        uint16_t *p = &s_fb[(size_t)(y + row) * LCD_W + (size_t)x];
        for (int col = 0; col < w; col++) {
            p[col] = c;
        }
    }
}

static void stroke_rect(int x, int y, int w, int h, uint16_t c)
{
    fill_rect(x, y, w, 1, c);
    fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c);
    fill_rect(x + w - 1, y, 1, h, c);
}

/* Rounded only where it registers: a 2 px notch off each corner, which at
 * this size reads as a radius without the cost of a real arc. */
static void fill_round_rect(int x, int y, int w, int h, uint16_t c)
{
    fill_rect(x + 2, y, w - 4, h, c);
    fill_rect(x, y + 2, 2, h - 4, c);
    fill_rect(x + w - 2, y + 2, 2, h - 4, c);
    fill_rect(x + 1, y + 1, 1, 1, c);
    fill_rect(x + w - 2, y + 1, 1, 1, c);
    fill_rect(x + 1, y + h - 2, 1, 1, c);
    fill_rect(x + w - 2, y + h - 2, 1, 1, c);
}

/* Solid disc, one scanline at a time. Comparing squared distances keeps it
 * in integers; at r=56 the whole thing is a few thousand stores. */
static void fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= r * r) {
            span++;
        }
        fill_rect(cx - span, cy + dy, span * 2 + 1, 1, c);
    }
}

/* An annulus, drawn as the difference of two spans per scanline so it stays
 * closed at every angle. A plotted outline leaves gaps where the curve runs
 * steep, which on a ring this size is a visible dashed line. */
static void fill_ring(int cx, int cy, int r_in, int r_out, uint16_t c)
{
    for (int dy = -r_out; dy <= r_out; dy++) {
        int outer = 0;
        while ((outer + 1) * (outer + 1) + dy * dy <= r_out * r_out) {
            outer++;
        }
        if (outer == 0) {
            continue;
        }
        if (dy * dy >= r_in * r_in) {
            fill_rect(cx - outer, cy + dy, outer * 2 + 1, 1, c);
            continue;
        }
        int inner = 0;
        while ((inner + 1) * (inner + 1) + dy * dy <= r_in * r_in) {
            inner++;
        }
        fill_rect(cx - outer, cy + dy, outer - inner, 1, c);
        fill_rect(cx + inner + 1, cy + dy, outer - inner, 1, c);
    }
}

static void draw_char(int x, int y, char ch, int scale, uint16_t c)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) {
        ch = ' ';
    }
    const uint8_t *g = s_font[(unsigned char)ch - FONT_FIRST];
    for (int col = 0; col < GLYPH_W; col++) {
        for (int row = 0; row < GLYPH_H; row++) {
            if (g[col] & (1U << row)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

static int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n == 0 ? 0 : n * (GLYPH_W + 1) * scale - scale;
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t c)
{
    for (; *s != '\0'; s++) {
        draw_char(x, y, *s, scale, c);
        x += (GLYPH_W + 1) * scale;
    }
}

static void draw_text_centered(int y, const char *s, int scale, uint16_t c)
{
    draw_text((LCD_W - text_width(s, scale)) / 2, y, s, scale, c);
}

/* ---- Shared state -------------------------------------------------------
 *
 * Written by whichever task saw the change, read by the render task. The
 * critical sections are a handful of stores, so a spinlock is both cheaper
 * and safer here than a mutex: nothing sleeps while holding it, so the OSDP
 * task can never end up parked behind the screen. */
typedef struct {
    bool     online;
    bool     reader_present;
    uint8_t  led_color;
    uint8_t  uid[10];
    uint8_t  uid_len;
} face_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static face_t       s_face;

void display_set_link(bool online)
{
    portENTER_CRITICAL(&s_lock);
    s_face.online = online;
    portEXIT_CRITICAL(&s_lock);
}

void display_set_reader_present(bool present)
{
    portENTER_CRITICAL(&s_lock);
    s_face.reader_present = present;
    portEXIT_CRITICAL(&s_lock);
}

void display_set_led(uint8_t osdp_color)
{
    portENTER_CRITICAL(&s_lock);
    s_face.led_color = osdp_color;
    portEXIT_CRITICAL(&s_lock);
}

void display_set_card(const uint8_t *uid, size_t uid_len)
{
    if (uid_len > sizeof(s_face.uid)) {
        uid_len = sizeof(s_face.uid);
    }
    portENTER_CRITICAL(&s_lock);
    if (uid != NULL && uid_len > 0) {
        memcpy(s_face.uid, uid, uid_len);
        s_face.uid_len = (uint8_t)uid_len;
    } else {
        s_face.uid_len = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ---- The face ----------------------------------------------------------- */

#define DISC_CX      (LCD_W / 2)
#define DISC_CY      152
#define DISC_R       56

/* Rows the disc and its caption own. Only these get pushed when nothing but
 * the colour moved — the common case, since the offline breath repaints 20
 * times a second and has no business resending the header and footer with
 * every step of the ramp. */
#define DISC_BAND_Y0 82
#define DISC_BAND_Y1 240

static const char *color_name(uint8_t c)
{
    switch (c) {
    case OSDP_LED_RED:     return "RED";
    case OSDP_LED_GREEN:   return "GREEN";
    case OSDP_LED_AMBER:   return "AMBER";
    case OSDP_LED_BLUE:    return "BLUE";
    case OSDP_LED_MAGENTA: return "MAGENTA";
    case OSDP_LED_CYAN:    return "CYAN";
    case OSDP_LED_WHITE:   return "WHITE";
    case OSDP_LED_BLACK:
    default:               return "OFF";
    }
}

static void render_disc(const face_t *f, uint8_t r, uint8_t g, uint8_t b)
{
    fill_rect(0, DISC_BAND_Y0, LCD_W, DISC_BAND_Y1 - DISC_BAND_Y0, C_BG);

    /* A permanent thin ring, so the LED still has a location on screen when
     * it is showing black. An unlit disc on a dark ground is otherwise
     * indistinguishable from nothing having been drawn at all. */
    fill_ring(DISC_CX, DISC_CY, DISC_R + 8, DISC_R + 9, C_LINE);

    bool lit = (r | g | b) != 0;
    if (lit) {
        /* Quarter-intensity halo, then the disc: a cheap stand-in for the
         * bloom a real LED throws onto its bezel. */
        fill_ring(DISC_CX, DISC_CY, DISC_R, DISC_R + 6,
                  RGB(r / 4, g / 4, b / 4));
        fill_circle(DISC_CX, DISC_CY, DISC_R, RGB(r, g, b));
    } else {
        fill_circle(DISC_CX, DISC_CY, DISC_R, C_OFFDISC);
    }

    const char *label = f->online ? color_name(f->led_color) : "LINK DOWN";
    draw_text_centered(DISC_CY + DISC_R + 22, label, 2,
                       f->online ? C_TEXT : C_MUTED);
}

static void render_frame(const face_t *f)
{
    char line[32];

    fill_rect(0, 0, LCD_W, LCD_H, C_BG);

    draw_text_centered(14, "OPENREADER", 2, C_TEXT);
    snprintf(line, sizeof(line), "OSDP PD  ADDR %d",
             CONFIG_OPENREADER_OSDP_ADDRESS);
    draw_text_centered(36, line, 1, C_MUTED);
    fill_rect((LCD_W - 30) / 2, 50, 30, 2, C_ACCENT);

    /* Card panel. */
    fill_round_rect(10, 250, LCD_W - 20, 44, C_PANEL);
    stroke_rect(10, 250, LCD_W - 20, 44, C_LINE);
    draw_text(20, 257, "CARD", 1, C_MUTED);

    if (f->uid_len > 0) {
        char hex[sizeof(f->uid) * 2 + 1];
        for (int i = 0; i < f->uid_len; i++) {
            snprintf(&hex[i * 2], 3, "%02X", f->uid[i]);
        }
        hex[f->uid_len * 2] = '\0';
        /* Two characters per UID byte outruns the panel past four bytes, so
         * a 7-byte card drops to the small face rather than running off the
         * edge. */
        int scale = (text_width(hex, 2) <= LCD_W - 40) ? 2 : 1;
        draw_text(20, scale == 2 ? 270 : 274, hex, scale, C_TEXT);
    } else {
        draw_text(20, 270, f->reader_present ? "NO CARD" : "NO READER", 2,
                  f->reader_present ? C_MUTED : C_LINE);
    }

    /* Status strip. */
    const char *pill = f->online ? "ONLINE" : "OFFLINE";
    fill_round_rect(10, 302, 60, 14, f->online ? C_OK : C_BAD);
    draw_text(10 + (60 - text_width(pill, 1)) / 2, 306, pill, 1, C_BG);

    snprintf(line, sizeof(line), "%d CLEAR", CONFIG_OPENREADER_RS485_BAUD);
    draw_text(LCD_W - 10 - text_width(line, 1), 306, line, 1, C_MUTED);

    render_disc(f, 0, 0, 0);
}

static void flush_band(int y0, int y1)
{
    (void)esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y1,
                                    &s_fb[(size_t)y0 * LCD_W]);
}

static void render_task(void *arg)
{
    (void)arg;

    face_t  shown;
    uint8_t shown_r = 0, shown_g = 0, shown_b = 0;
    bool    first = true;

    memset(&shown, 0, sizeof(shown));

    for (;;) {
        face_t now;
        portENTER_CRITICAL(&s_lock);
        now = s_face;
        portEXIT_CRITICAL(&s_lock);

        uint8_t r, g, b;
        status_led_current_rgb(&r, &g, &b);

        bool face_moved  = first || memcmp(&now, &shown, sizeof(now)) != 0;
        bool color_moved = first || r != shown_r || g != shown_g ||
                           b != shown_b;

        if (face_moved) {
            render_frame(&now);
            render_disc(&now, r, g, b);
            flush_band(0, LCD_H);
        } else if (color_moved) {
            render_disc(&now, r, g, b);
            flush_band(DISC_BAND_Y0, DISC_BAND_Y1);
        }

        shown   = now;
        shown_r = r;
        shown_g = g;
        shown_b = b;
        first   = false;

        /* 20 fps is set by the offline breath, the only thing here that
         * animates. Everything else changes at human speed and repaints on
         * the frame it changes. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t display_init(void)
{
    /* One full frame, DMA-capable: 110 KB out of the ~400 KB this app has
     * to play with. Paid once, and it buys flicker-free compositing — the
     * alternative, drawing straight at the panel, shows every intermediate
     * rectangle as the frame is assembled. */
    s_fb = heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(uint16_t),
                            MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_fb != NULL, ESP_ERR_NO_MEM, TAG,
                        "framebuffer (%d bytes)",
                        LCD_W * LCD_H * (int)sizeof(uint16_t));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BOARD_LCD_DC,
        .cs_gpio_num       = BOARD_LCD_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST,
                                 &io_cfg, &io),
        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel),
                        TAG, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    /* This panel is wired for inverted colour. Without this every shade
     * comes out as its complement and the dark theme renders as white. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),
                        TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP),
                        TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "display on");

    /* Backlight last. A panel lit before it is initialised shows a screenful
     * of uninitialised RAM first. */
    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    gpio_set_level(BOARD_LCD_BL, 1);

    /* Priority 3 against the OSDP task's 10. A frame is tens of
     * milliseconds of SPI and must never be what an ACU is waiting on. */
    if (xTaskCreate(render_task, "display", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST7789 %dx%d up on SPI2 (SCLK=%d MOSI=%d CS=%d DC=%d)",
             LCD_W, LCD_H, BOARD_LCD_SCLK, BOARD_LCD_MOSI, BOARD_LCD_CS,
             BOARD_LCD_DC);
    return ESP_OK;
}
