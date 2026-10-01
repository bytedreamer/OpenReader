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
 * Sleep. When CONFIG_OPENREADER_DISPLAY_SLEEP is built the panel blanks
 * after CONFIG_OPENREADER_DISPLAY_SLEEP_MS with nothing on the face
 * changing, and comes back on the next change. No caller has to ask for
 * that or know about it: every setter below already reports a change, and a
 * change is the whole definition of activity here.
 *
 * The panel has hardware SPI2 to itself. It is the one peripheral here that
 * cannot be moved — its clock and data are soldered to GPIO7/6 — so when the
 * display is built it owns the host, and an RC522 alongside it is clocked in
 * software instead (CONFIG_OPENREADER_RC522_BUS). See board.h.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include "credential.h"

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How this PD is talking to the ACU right now.
 *
 * Six states and not a bool, because a person standing at the door with a
 * laptop has to be able to tell apart facts that all look like "the Secure
 * Channel light is not green":
 *
 *   this build has no crypto bound      a property of the firmware
 *   the key store is damaged            a property of this unit
 *   nobody has keyed the reader yet     a property of the installation
 *   the ACU and the reader disagree     almost always a key mismatch
 *
 * The two install states are split from the two operational ones for a
 * reason that is easy to get wrong. A session established on SCBK-D is
 * cryptographically identical to a real one and completely worthless: the
 * key is a constant printed in the specification, so anyone on the bus can
 * decrypt the traffic and mint their own. A face that showed that as SECURE
 * would be reporting the mechanism instead of the security, and the state
 * an installer most needs to notice — "I configured this and then forgot to
 * key it" — is precisely the one it would hide. */
typedef enum {
    DISPLAY_SC_CLEAR = 0,   /* no Secure Channel configured in this build  */
    DISPLAY_SC_INSTALL,     /* install mode: SCBK-D, no session yet        */
    DISPLAY_SC_INSTALL_UP,  /* install mode: session established on SCBK-D */
    DISPLAY_SC_NONE,        /* keyed, but no session established           */
    DISPLAY_SC_ACTIVE,      /* keyed, SCS_14 done; traffic is wrapped      */
    DISPLAY_SC_FAULT,       /* a key is stored and will not come back      */
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

/* Take the whole face over while the key-reset button is being held.
 *
 * `seconds_left` counts down to the erase; DISPLAY_NO_KEY_RESET releases the
 * screen. Quantised to whole seconds by the caller rather than shown as a
 * smooth bar, because the panel repaints only when its snapshot changes and
 * a per-millisecond value would mean a full 110 KB frame on every tick.
 *
 * A takeover rather than another row. Someone holding a button for ten
 * seconds is asking a question — "is this doing anything?" — and the answer
 * has to be visible from wherever they are standing, not tucked into a panel
 * beneath the baud rate. It is also the last warning before a working
 * reader's key is destroyed, which is worth a screen of its own. */
#define DISPLAY_NO_KEY_RESET (-1)
void display_set_key_reset(int seconds_left);

/* Show a card. NULL clears back to the idle state.
 *
 * The two kinds are shown differently, on purpose.
 *
 * A UID is displayed and stays up. It is not a secret in any useful sense —
 * anyone with a phone can read it off the badge — and seeing it is how you
 * tell whether the reader read the card you think it did.
 *
 * A PKOC credential is never displayed, and the fact of it is displayed only
 * briefly. The credential is the value that names a person, and a screen at a
 * door showing it is a transcript legible to whoever is standing behind the
 * holder. So the panel shows one word, large enough to read at a glance from
 * where the holder is standing, and clears itself a few seconds later —
 * because "a PKOC card was read" is a report about something that just
 * happened, and leaving it up turns it into a claim about the present. */
void display_set_card(const credential_t *cred);

#endif /* DISPLAY_H */
