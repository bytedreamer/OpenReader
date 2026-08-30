/* The reader face on the board's 1.47" LCD.
 *
 * This is the virtual reader: a colour disc that mirrors, pixel for pixel,
 * whatever the WS2812 is showing — the ACU's osdp_LED colour while the link
 * is up, the blue breath while it is down — plus the identity and link
 * state a person standing at the door would want to see. It is a display of
 * state that already exists; nothing here originates a reading.
 *
 * Threading. Every setter below is safe from any task and does no I/O: it
 * copies into a small shared snapshot under a spinlock and returns. A
 * private task owns the panel and repaints only when the snapshot it
 * rendered last differs from the current one. That split matters — a full
 * frame is 110 KB over SPI, and the OSDP task cannot afford to sit behind
 * it. Nothing on the bus path ever blocks on the screen.
 *
 * The panel has hardware SPI2 to itself. It is the one peripheral here that
 * cannot be moved — its clock and data are soldered to GPIO7/6 — so when the
 * display is built it owns the host, and an RC522 alongside it is clocked in
 * software instead (CONFIG_OPENREADER_RC522_BUS). See board.h.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bring up the panel, light the backlight, paint the first frame and start
 * the repaint task. Call after status_led_init() — the disc reads its
 * colour from there. */
esp_err_t display_init(void);

/* Link state, as the OSDP task sees it. Drives the status pill and, through
 * status_led, the disc. */
void display_set_link(bool online);

/* The colour the ACU last commanded, used for the caption under the disc.
 * The disc itself takes its colour from status_led rather than from here,
 * so that what the screen shows and what the WS2812 shows cannot drift
 * apart; this is only the name for it. */
void display_set_led(uint8_t osdp_color);

/* Whether a card reader is attached at all. False puts the card panel into
 * "NO READER" rather than "NO CARD" — an idle reader and an absent one look
 * the same otherwise, and that is exactly the confusion this board is
 * currently in. */
void display_set_reader_present(bool present);

/* Show a card. uid_len 0 clears back to the idle state. Call this from
 * wherever real reads arrive once an RC522 is wired. */
void display_set_card(const uint8_t *uid, size_t uid_len);

#endif /* DISPLAY_H */
