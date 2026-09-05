/* The physical way back to install mode.
 *
 * osdp_KEYSET is a one-way door. Once the ACU has given this reader an
 * operational key, the reader refuses SCBK-D — it has to, or being keyed
 * would mean nothing — and from then on the only party that can talk to it
 * is one that already holds the key. Which is exactly right, and exactly the
 * problem when the panel is replaced, the key is lost, or a reader comes off
 * one site and goes onto another. Something has to be able to say "forget
 * it", and that something must not be reachable over the wire, because a
 * remote path back to a published key is not a recovery mechanism, it is the
 * hole the key was there to close.
 *
 * So it is a button, held down, for a long time.
 *
 * ---- Why the BOOT button ----
 *
 * GPIO9 on this carrier, and no new parts. It is worth being clear that this
 * does not disturb what BOOT already does. The strapping pin is sampled by
 * the ROM at reset: holding it across a reset still drops the board into
 * download mode, the key untouched, exactly as before. This reads the same
 * pin as an ordinary input long after that sampling is over, while the
 * firmware is running and polling the bus. The two uses never overlap.
 *
 * Once the board is in an enclosure the button is inside it, which is the
 * property that matters: reaching it means opening the box, and on a build
 * with the tamper switch fitted that is an event the ACU is told about. The
 * reader cannot be quietly downgraded — either the head end sees the
 * enclosure open, or nobody got to the button.
 *
 * ---- Why ten seconds ----
 *
 * Long enough that it cannot be an accident, and long enough that someone
 * who meant to press RESET and hit BOOT does not wipe a working reader's
 * key. Deliberately not so long that an installer up a ladder gives up
 * before it fires. The hold restarts from zero on any release, so it has to
 * be one continuous press.
 *
 * ---- Not built by default ----
 *
 * A reader with no Secure Channel has no key to reset, and this module then
 * does nothing but hold a pin. It follows CONFIG_OPENREADER_SECURE_CHANNEL.
 */
#ifndef KEY_RESET_H
#define KEY_RESET_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Configure the pin. Safe to call before the OSDP loop starts; it samples
 * nothing that matters until key_reset_poll() runs. */
esp_err_t key_reset_init(void);

/* Sample the button and advance the hold timer.
 *
 * Call from the OSDP loop. Like tamper_poll() it is a register read and a
 * comparison, cheap enough for every tick. Returns true exactly once, on the
 * tick where a continuous hold has reached the full duration — the caller's
 * cue to erase the key. It will not fire again until the button has been
 * released, so a finger left on it does not repeat.
 *
 * The debounce is the same shape as tamper.c's and exists for a different
 * reason: nothing downstream is spammed by a bouncing button, but a contact
 * that chatters once at the eight-second mark would otherwise restart a hold
 * the installer believes is nearly finished. */
bool key_reset_poll(void);

/* How long the button has been continuously held, in milliseconds, or 0 when
 * it is not held.
 *
 * For feedback while the hold is in progress. Ten seconds of nothing
 * happening is indistinguishable from a button that is not wired, and an
 * installer with no signal will let go at six seconds and conclude the
 * feature is broken. */
uint32_t key_reset_held_ms(void);

/* The full hold duration, so a caller can render progress against it without
 * duplicating the Kconfig symbol. */
uint32_t key_reset_hold_ms(void);

#endif /* KEY_RESET_H */
