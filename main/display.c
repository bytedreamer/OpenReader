#include "display.h"
#include "board.h"

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
#define C_TEXT     RGB(0xE6, 0xEA, 0xF2)
#define C_MUTED    RGB(0x78, 0x84, 0x9A)
#define C_ACCENT   RGB(0x4C, 0x8D, 0xFF)
#define C_OK       RGB(0x3D, 0xDC, 0x84)
#define C_WARN     RGB(0xFF, 0xB0, 0x3C)
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
    uint8_t  sc;             /* display_sc_t */
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

void display_set_secure(display_sc_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_face.sc = (uint8_t)state;
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

/* The link panel: three rows in the space the LED disc used to occupy.
 *
 * The disc is gone deliberately. It mirrored a WS2812 sitting a centimetre
 * away, so the screen spent its largest area repeating something already on
 * the board — and repainted twenty times a second to keep the offline breath
 * in step with it. What it never showed is what nobody can read off an LED
 * and everybody asks first when a reader will not talk: the speed, the
 * address, and whether the traffic is encrypted. Those three carry the space
 * now, and all of them change at human speed.
 */
#define LINK_X        10
#define LINK_W        (LCD_W - 20)
#define LINK_ROW_H    44
#define LINK_ROW_GAP  12
#define LINK_Y0       78

#define LINK_ROW_Y(i) (LINK_Y0 + (i) * (LINK_ROW_H + LINK_ROW_GAP))

/* One labelled row: caption small and muted, value large beneath it, and an
 * optional unit or gloss trailing the value at caption size. The 7 px / 20 px
 * offsets are the card panel's, which is what makes the four panels read as
 * one column rather than four decisions. */
static void draw_row(int y, const char *label, const char *value,
                     uint16_t value_color, const char *note)
{
    fill_round_rect(LINK_X, y, LINK_W, LINK_ROW_H, C_PANEL);
    stroke_rect(LINK_X, y, LINK_W, LINK_ROW_H, C_LINE);
    draw_text(LINK_X + 10, y + 7, label, 1, C_MUTED);
    draw_text(LINK_X + 10, y + 20, value, 2, value_color);
    if (note != NULL) {
        /* Sat on the value's baseline, not its cap height: 7 px glyphs hung
         * from the top of a 14 px line read as a superscript. */
        draw_text(LINK_X + 10 + text_width(value, 2) + 6, y + 27, note, 1,
                  C_MUTED);
    }
}

/* Clear text is drawn in the warning colour rather than the neutral one. It
 * is the correct state for this build — no crypto vtable is bound — but it
 * is not a state anything should be deployed in, and a face that showed it
 * as calmly as the baud rate would be helping it go unnoticed. */
static void draw_secure_row(const face_t *f)
{
    const char *value;
    uint16_t    color;

    switch (f->sc) {
    case DISPLAY_SC_ACTIVE:
        value = "SECURE";
        color = C_OK;
        break;
    case DISPLAY_SC_NONE:
        /* Keys are configured and the ACU has not established a session:
         * almost always an SCBK mismatch, occasionally an ACU that has not
         * been told to use Secure Channel at all. */
        value = "NO SESSION";
        color = C_WARN;
        break;
    case DISPLAY_SC_CLEAR:
    default:
        value = "CLEAR TEXT";
        color = C_WARN;
        break;
    }
    draw_row(LINK_ROW_Y(2), "SECURE CHANNEL", value, color, NULL);
}

static void render_frame(const face_t *f)
{
    char line[32];
    char note[8];

    fill_rect(0, 0, LCD_W, LCD_H, C_BG);

    draw_text_centered(14, "OPENREADER", 2, C_TEXT);
    draw_text_centered(36, "OSDP PD", 1, C_MUTED);
    fill_rect((LCD_W - 30) / 2, 50, 30, 2, C_ACCENT);

    /* The terms of the link. Both of these are build-time settings rather
     * than anything negotiated on the wire — this PD binds no osdp_COMSET
     * handler, so what Kconfig says is what the UART is actually running. */
    snprintf(line, sizeof(line), "%d", CONFIG_OPENREADER_RS485_BAUD);
    draw_row(LINK_ROW_Y(0), "SPEED", line, C_TEXT, "BAUD");

    /* Decimal and hex together. Configuration screens ask for one, wire
     * traces show the other, and mistaking 0x14 for 14 is a whole evening.
     * The "0x" comes out as "0X" — the font has no lowercase. */
    snprintf(line, sizeof(line), "%d", CONFIG_OPENREADER_OSDP_ADDRESS);
    snprintf(note, sizeof(note), "0x%02X", CONFIG_OPENREADER_OSDP_ADDRESS);
    draw_row(LINK_ROW_Y(1), "ADDRESS", line, C_TEXT, note);

    draw_secure_row(f);

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

    /* Status strip. Full width and centred: with the disc gone this is the
     * only thing on the face saying whether anyone is polling us, and it has
     * to carry that on its own from across a corridor. */
    const char *pill = f->online ? "ONLINE" : "OFFLINE";
    fill_round_rect(10, 302, LCD_W - 20, 14, f->online ? C_OK : C_BAD);
    draw_text_centered(306, pill, 1, C_BG);
}

/* Push the composed frame to the panel. The draw_bitmap window is
 * end-exclusive, so LCD_H here is one past the last row. */
static void flush_frame(void)
{
    (void)esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

static void render_task(void *arg)
{
    (void)arg;

    face_t shown;
    bool   first = true;

    memset(&shown, 0, sizeof(shown));

    for (;;) {
        face_t now;
        portENTER_CRITICAL(&s_lock);
        now = s_face;
        portEXIT_CRITICAL(&s_lock);

        /* Nothing on this face animates any more — the breath went back to
         * the LED with the disc — so there is one dirty test and one full
         * repaint, and an unchanged reader costs a struct compare every
         * 50 ms. That is also why the partial-band flush is gone: the only
         * repaints left are the ones where the whole layout may have moved.
         *
         * memcmp over the struct is safe here because every field is
         * assigned individually into a static (so zero-initialised) copy;
         * the padding never holds anything but the zeros it started with. */
        if (first || memcmp(&now, &shown, sizeof(now)) != 0) {
            render_frame(&now);
            flush_frame();
        }

        shown = now;
        first = false;

        /* A repaint latency, not a frame rate: 50 ms is how long the face
         * can lag a card read or a link transition, which is well under what
         * anyone notices standing at a door. */
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
