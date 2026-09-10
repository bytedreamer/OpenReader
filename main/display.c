#include "display.h"
#include "board.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
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
    bool         online;
    bool         reader_present;
    uint8_t      sc;         /* display_sc_t */
    bool         has_card;
    credential_t card;
    uint32_t     read_ms;    /* uptime when the card was read */
    int8_t       key_reset;  /* seconds left, or DISPLAY_NO_KEY_RESET */
} face_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Every other member's "nothing yet" is zero; the key-reset countdown's is
 * not, and a zeroed one would mean "erasing now". Initialised here rather
 * than in display_init() so it cannot depend on the order two modules happen
 * to start in. */
static face_t       s_face = { .key_reset = DISPLAY_NO_KEY_RESET };

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

void display_set_key_reset(int seconds_left)
{
    portENTER_CRITICAL(&s_lock);
    s_face.key_reset = (int8_t)seconds_left;
    portEXIT_CRITICAL(&s_lock);
}

void display_set_card(const credential_t *cred)
{
    portENTER_CRITICAL(&s_lock);
    /* Zeroed and then filled field by field rather than assigned wholesale.
     * The render task's dirty check is a memcmp over the whole face, and a
     * struct assignment is free to leave whatever it likes in the padding
     * between members — which would show up as a repaint on every pass. */
    memset(&s_face.card, 0, sizeof(s_face.card));
    s_face.read_ms  = 0;
    s_face.has_card = (cred != NULL && cred->len > 0);
    if (s_face.has_card) {
        memcpy(s_face.card.bytes, cred->bytes, cred->len);
        s_face.card.len       = cred->len;
        s_face.card.bit_count = cred->bit_count;
        s_face.card.kind      = cred->kind;
        s_face.card.verified  = cred->verified;
        s_face.read_ms = (uint32_t)(esp_timer_get_time() / 1000);
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

/* Exactly one of these is drawn in the neutral "this is fine" colour, and it
 * is the only one that has earned it.
 *
 * Clear text is the correct state for a build with no crypto bound, and the
 * install states are the correct state for a reader that has not been keyed
 * yet — but none of the three is a state anything should be left in on a
 * door, and a face that showed them as calmly as the baud rate would be
 * helping them go unnoticed. So they are all drawn as warnings, and the
 * gloss beside each says which of them this is. */
static void draw_secure_row(const face_t *f)
{
    const char *value;
    const char *note  = NULL;
    uint16_t    color = C_WARN;

    switch (f->sc) {
    case DISPLAY_SC_ACTIVE:
        value = "SECURE";
        color = C_OK;
        break;
    case DISPLAY_SC_NONE:
        /* Keyed, and the ACU has not established a session: almost always an
         * SCBK mismatch, occasionally an ACU that has not been told to use
         * Secure Channel at all. */
        value = "NO SESSION";
        break;
    case DISPLAY_SC_INSTALL:
        /* Waiting to be keyed. The gloss names the key rather than saying
         * "default", because SCBK-D is the term the panel's own
         * documentation will use for the thing to send osdp_KEYSET over. */
        value = "INSTALL";
        note  = "SCBK-D";
        break;
    case DISPLAY_SC_INSTALL_UP:
        /* A session is up, wrapped under a key from the specification. The
         * word that matters here is not SECURE, which is what the mechanism
         * would say, but that the key is public — so the gloss says so and
         * the colour stays a warning. */
        value = "INSTALL";
        note  = "OPEN KEY";
        break;
    case DISPLAY_SC_FAULT:
        /* Something is stored and it would not come back. Deliberately not
         * shown as INSTALL: the reader is refusing SCBK-D, so an installer
         * told it was in install mode would spend the afternoon on a
         * handshake that is never going to complete. See sc_key.h. */
        value = "KEY FAULT";
        color = C_BAD;
        break;
    case DISPLAY_SC_CLEAR:
    default:
        value = "CLEAR TEXT";
        break;
    }
    draw_row(LINK_ROW_Y(2), "SECURE CHANNEL", value, color, note);
}

/* The card panel.
 *
 * What it shows depends on which kind of credential arrived, and the
 * difference is deliberate rather than incidental.
 *
 * A UID is shown, and stays. It is not a secret in any useful sense — anyone
 * with a phone can read it off the badge — and seeing it is how you confirm
 * the reader read the card you meant.
 *
 * A PKOC credential is never shown. It is the value that names a person, and
 * sixty-four characters of it on a screen at a door is a transcript readable
 * by whoever is standing behind the holder. One word goes up instead, big
 * enough to read at arm's length without leaning in, and it clears itself
 * after a few seconds: "a PKOC card was read" is a report about something
 * that just happened, and leaving it up would turn it into a claim about
 * whoever is standing there now.
 */
#define CARD_X 10
#define CARD_Y 244
#define CARD_W (LCD_W - 20)
#define CARD_H 54

/* How long a PKOC read stays on the face. Long enough to look up and read it
 * after presenting a card, short enough that the panel is not still saying it
 * when the next person arrives. */
#define CARD_PKOC_HOLD_MS 5000U

/* How much hex fits across the panel. At scale 1 a character is six pixels
 * wide, at scale 2 twelve; both counts are even so a row never breaks in the
 * middle of a byte. */
#define CARD_ROW_CHARS   24
#define CARD_BIG_CHARS   12

static void draw_card_panel(const face_t *f)
{
    fill_round_rect(CARD_X, CARD_Y, CARD_W, CARD_H, C_PANEL);
    stroke_rect(CARD_X, CARD_Y, CARD_W, CARD_H, C_LINE);
    draw_text(CARD_X + 10, CARD_Y + 7, "CARD", 1, C_MUTED);

    if (!f->has_card) {
        draw_text(CARD_X + 10, CARD_Y + 24,
                  f->reader_present ? "NO CARD" : "NO READER", 2,
                  f->reader_present ? C_MUTED : C_LINE);
        return;
    }

    if (f->card.kind == CREDENTIAL_PKOC) {
        /* One word, at four times the base glyph — the largest thing on the
         * face, and the only text here meant to be read from where the card
         * holder is standing rather than from a bench.
         *
         * The colour is the one piece of nuance kept. A build with signature
         * verification switched off still produces PKOC credentials, and
         * rendering those exactly like a proven one would make the panel
         * quietly complicit in the thing that configuration is dangerous
         * for. Same word, different colour; the boot log says the rest. */
        draw_text(CARD_X + 10, CARD_Y + 18, "PKOC", 4,
                  f->card.verified ? C_OK : C_WARN);
        return;
    }

    char   hex[CREDENTIAL_MAX_BYTES * 2 + 1];
    size_t len = credential_hex(&f->card, hex, sizeof(hex));

    if (len <= CARD_BIG_CHARS) {
        draw_text(CARD_X + 10, CARD_Y + 24, hex, 2, C_TEXT);
        return;
    }

    char row[CARD_ROW_CHARS + 1];

    size_t head = (len < CARD_ROW_CHARS) ? len : CARD_ROW_CHARS;
    memcpy(row, hex, head);
    row[head] = '\0';
    draw_text(CARD_X + 10, CARD_Y + 22, row, 1, C_TEXT);

    if (len <= CARD_ROW_CHARS) {
        return;
    }

    const size_t rest = len - head;
    if (rest <= CARD_ROW_CHARS) {
        memcpy(row, &hex[head], rest);
        row[rest] = '\0';
    } else {
        /* Two dots and the tail. The gap is marked rather than silently
         * dropped: showing a head and a tail as if they were consecutive
         * would invite a wrong comparison. */
        const size_t tail = CARD_ROW_CHARS - 2U;
        row[0] = '.';
        row[1] = '.';
        memcpy(&row[2], &hex[len - tail], tail);
        row[CARD_ROW_CHARS] = '\0';
    }
    draw_text(CARD_X + 10, CARD_Y + 34, row, 1, C_TEXT);
}

/* The whole screen, while the key-reset button is held.
 *
 * Written to be read by someone whose finger is on the button and who wants
 * to know two things: that the press is registering, and how long until it
 * is too late to change their mind. The count is the largest thing on the
 * panel for that second reason — this is a destructive operation with an
 * undo that lasts exactly as long as the number is still counting.
 *
 * Deliberately says KEY RESET and not FACTORY RESET. Nothing else is
 * touched: the address, the baud rate and every other setting are build-time
 * and survive, and someone who believed they were clearing the whole device
 * would be surprised in both directions. */
static void render_key_reset(const face_t *f)
{
    char count[4];

    fill_rect(0, 0, LCD_W, LCD_H, C_BG);

    draw_text_centered(60, "KEY RESET", 2, C_WARN);
    draw_text_centered(84, "HOLD TO CONFIRM", 1, C_MUTED);

    snprintf(count, sizeof(count), "%d", f->key_reset > 0 ? f->key_reset : 0);
    draw_text_centered(130, count, 6, C_BAD);

    draw_text_centered(196, "RELEASE TO", 1, C_MUTED);
    draw_text_centered(208, "CANCEL", 1, C_MUTED);

    /* What is about to be lost, in the terms the installer will next have to
     * act in: the reader goes back to answering the published install key,
     * and the panel has to re-key it. */
    fill_round_rect(10, 240, LCD_W - 20, 44, C_PANEL);
    stroke_rect(10, 240, LCD_W - 20, 44, C_LINE);
    draw_text_centered(250, "ERASES THE SCBK", 1, C_TEXT);
    draw_text_centered(266, "BACK TO SCBK-D", 1, C_WARN);
}

static void render_frame(const face_t *f)
{
    char line[32];
    char note[8];

    if (f->key_reset != DISPLAY_NO_KEY_RESET) {
        render_key_reset(f);
        return;
    }

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

    draw_card_panel(f);

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

/* ---- Screen sleep -------------------------------------------------------
 *
 * A reader at a door spends nearly all of its life showing a face nobody is
 * looking at. After CONFIG_OPENREADER_DISPLAY_SLEEP_MS with nothing on the
 * face changing, the panel goes dark; the next thing that changes it brings
 * it back.
 *
 * Inactivity is defined as the render task's own dirty test, and
 * deliberately not as a list of events worth waking for. A separate list
 * would be a second opinion about what matters on this screen, free to drift
 * out of step with the first one — and the first one is the one that decides
 * what is actually on the glass. If the frame the panel is showing is still
 * the correct frame, there is nothing here for anyone to look at. Everything
 * that reaches the face therefore counts: a link transition, a Secure
 * Channel change, the key-reset countdown, a card read — including a re-read
 * of the same card, which changes no digit on the panel but does move
 * read_ms, so a card sitting on the antenna keeps the screen up.
 *
 * Both halves are switched, for two different reasons. The backlight is
 * where nearly all of the current goes and is what makes the screen visibly
 * off. SLPIN on top of it stops the controller's own oscillator, booster and
 * panel drive, which is worth having on a device that may be on a door for a
 * decade. The vendor driver holds the 100 ms the controller wants either
 * side of that command itself; it lands on this task, which is priority 3
 * and on nothing's critical path.
 */
#if CONFIG_OPENREADER_DISPLAY_SLEEP

#define SLEEP_AFTER_US ((int64_t)CONFIG_OPENREADER_DISPLAY_SLEEP_MS * 1000)

static bool    s_asleep;
static bool    s_waking;
static int64_t s_idle_since_us;

/* Called before every repaint, asleep or not: a repaint only happens when
 * the face changed, which is exactly what activity means here.
 *
 * If the panel is asleep this takes it out of sleep but leaves it dark, so
 * the caller can compose the current frame into a panel nobody is looking
 * at. Waking the screen first would light the frame it went to sleep with
 * for as long as the repaint takes — a stale face, briefly, which on a
 * screen whose whole job is reporting state is the one artefact worth an
 * ordering rule. */
static void screen_before_paint(void)
{
    s_idle_since_us = esp_timer_get_time();
    if (!s_asleep) {
        return;
    }
    (void)esp_lcd_panel_disp_sleep(s_panel, false);
    s_asleep = false;
    s_waking = true;
}

/* Called after the frame has been pushed. Lights the panel only if this
 * repaint was the one that woke it; an ordinary repaint touches nothing. */
static void screen_after_paint(void)
{
    if (!s_waking) {
        return;
    }
    (void)esp_lcd_panel_disp_on_off(s_panel, true);
    gpio_set_level(BOARD_LCD_BL, 1);
    s_waking = false;
    ESP_LOGI(TAG, "screen awake");
}

/* Called on ticks where the face did not change. */
static void screen_idle_tick(void)
{
    if (s_asleep ||
        (esp_timer_get_time() - s_idle_since_us) < SLEEP_AFTER_US) {
        return;
    }
    gpio_set_level(BOARD_LCD_BL, 0);
    (void)esp_lcd_panel_disp_on_off(s_panel, false);
    (void)esp_lcd_panel_disp_sleep(s_panel, true);
    s_asleep = true;
    ESP_LOGI(TAG, "screen asleep: the face has not changed for %u ms",
             (unsigned)CONFIG_OPENREADER_DISPLAY_SLEEP_MS);
}

#else  /* the panel simply stays lit */

static inline void screen_before_paint(void) { }
static inline void screen_after_paint(void)  { }
static inline void screen_idle_tick(void)    { }

#endif

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

        /* Expire a PKOC read out of the snapshot before the dirty check
         * below, so it costs exactly one repaint when the window closes and
         * nothing at all before or after. Doing it here rather than with a
         * timer somewhere is what keeps the render task the only thing that
         * decides when the panel changes. */
        if (now.has_card && now.card.kind == CREDENTIAL_PKOC) {
            const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
            if ((ms - now.read_ms) >= CARD_PKOC_HOLD_MS) {
                memset(&now.card, 0, sizeof(now.card));
                now.has_card = false;
                now.read_ms  = 0;
            }
        }

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
            screen_before_paint();
            render_frame(&now);
            flush_frame();
            screen_after_paint();
        } else {
            screen_idle_tick();
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
