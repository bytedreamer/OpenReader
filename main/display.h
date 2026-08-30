/* The reader face on the board's 1.47" LCD.
 *
 * The screen shows what the WS2812 cannot say. The LED already carries the
 * ACU's osdp_LED colour while the link is up and a blue breath while it is
 * down, so the panel does not mirror it — it shows the link instead: the
 * terms this PD is talking on (speed, address, Secure Channel) and the last
 * card read. It is a display of state that already exists; nothing here
 * originates a reading.
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

/* How this PD is talking to the ACU right now.
 *
 * Three states and not a bool, because "this build has no Secure Channel"
 * and "Secure Channel is configured but no session is up" are different
 * facts about the same reader, and a person standing at the door with a
 * laptop needs to tell them apart. The first is a property of the firmware,
 * the second is usually a key mismatch. */
typedef enum {
    DISPLAY_SC_CLEAR = 0,   /* no Secure Channel configured in this build */
    DISPLAY_SC_NONE,        /* configured, but no session established     */
    DISPLAY_SC_ACTIVE,      /* SCS_14 done; traffic is wrapped            */
} display_sc_t;

/* Bring up the panel, light the backlight, paint the first frame and start
 * the repaint task. */
esp_err_t display_init(void);

/* Link state, as the OSDP task sees it. Drives the status pill. */
void display_set_link(bool online);

/* Secure Channel state, as the OSDP task sees it. */
void display_set_secure(display_sc_t state);

/* Whether a card reader is attached at all. False puts the card panel into
 * "NO READER" rather than "NO CARD" — an idle reader and an absent one look
 * the same otherwise, and that is exactly the confusion this board is
 * currently in. */
void display_set_reader_present(bool present);

/* Show a card. uid_len 0 clears back to the idle state. Call this from
 * wherever real reads arrive once an RC522 is wired. */
void display_set_card(const uint8_t *uid, size_t uid_len);

#endif /* DISPLAY_H */
