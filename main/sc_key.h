/* Where the Secure Channel Base Key lives between power cycles.
 *
 * OSDP's SCBK is the root of everything the Secure Channel promises. An
 * attacker who reads it can decrypt every credential that has ever crossed
 * the bus and impersonate the ACU to the reader or the reader to the ACU.
 * So this module has two jobs, and the second one is the hard one: hold the
 * key across a reboot, and make a copy of the flash worth as little as
 * possible to whoever takes one.
 *
 * ---- The three states a reader can be in ----
 *
 * SC_KEY_NONE        Nothing has ever been stored. The reader is in install
 *                    mode and answers a handshake on SCBK-D, the well-known
 *                    key from spec D.4 that every OSDP implementation knows.
 *                    This is not security; it is the state in which security
 *                    gets configured. A reader left here is a reader anyone
 *                    on the bus can talk to.
 *
 * SC_KEY_LOADED      An operational key came back out of the store. The
 *                    reader answers on that key alone and refuses SCBK-D —
 *                    which is the point of having been keyed. See
 *                    osdp_reader.c, which is where that refusal is enacted.
 *
 * SC_KEY_UNREADABLE  Something is stored and it would not come back. A
 *                    corrupted blob, or flash lifted onto a different chip
 *                    than the one whose eFuse wrapped it.
 *
 * That third state exists on purpose, and it is worth being explicit about
 * why it does not simply fall back to install mode. A reader that answered
 * SCBK-D whenever its key store looked damaged would hand anyone who can
 * scribble on flash a downgrade to a published key — corrupt one blob and
 * the door is speaking a key printed in the specification. So an unreadable
 * store is a fault, reported and refused, and the only way back to install
 * mode is the deliberate physical act in key_reset.h.
 *
 * ---- What actually protects the key ----
 *
 * The ESP32-C6 has an HMAC peripheral wired to read-protected eFuse key
 * blocks: software can ask the hardware to compute HMAC-SHA256 under one of
 * those keys but cannot read the key itself, ever, by any interface. So the
 * SCBK is stored encrypted under a key derived that way, and the wrapping
 * key exists nowhere in flash and nowhere in RAM for longer than one call.
 * Someone who desolders the flash and dumps it gets a ciphertext that only
 * that one chip can open.
 *
 * That needs an eFuse key block burned for HMAC-upstream use — one esptool
 * command, and not the one-way, whole-device commitment that turning on
 * flash encryption is:
 *
 *     head -c 32 /dev/urandom > hmac_key.bin
 *     espefuse.py burn_key BLOCK_KEY0 hmac_key.bin HMAC_UP
 *
 * Then delete hmac_key.bin. Nothing ever needs it again — the value is not
 * an escrow of anything, it is just entropy the chip now holds and will not
 * give back, and the SCBK is recoverable from the ACU by re-keying rather
 * than from a backup.
 *
 * With no such block burned the key is stored in the clear and every boot
 * says so at ESP_LOGE. A bare board still works, which is what makes it
 * possible to bring Secure Channel up on the bench before committing an
 * eFuse — but a reader on a door in that state is one esptool read-flash
 * away from having no Secure Channel at all.
 *
 * Burning the block later is not too late: a key found stored in the clear
 * is rewrapped on the next load, so provisioning the eFuse on an already
 * deployed reader protects the key it is already carrying.
 *
 * ---- What this does NOT protect against ----
 *
 * The wrapping binds the key to the chip, not to the firmware. Anything that
 * can run code on this ESP32-C6 can ask the HMAC peripheral to unwrap the
 * blob, because that is exactly what this module does. Closing that gap is
 * what Secure Boot and flash encryption are for, and they remain the right
 * answer for a reader guarding anything that matters. This raises the floor
 * from "readable with esptool" to "requires code execution on this specific
 * device"; it does not replace the ceiling.
 */
#ifndef SC_KEY_H
#define SC_KEY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* The SCBK is AES-128: sixteen bytes, and the library's OSDP_SC_KEY_LEN
 * agrees. Restated here so this header does not drag the OSDP headers into
 * everything that includes it. A static assertion in sc_key.c checks the two
 * have not drifted. */
#define SC_KEY_LEN 16U

typedef enum {
    SC_KEY_NONE = 0,    /* nothing stored — install mode                   */
    SC_KEY_LOADED,      /* an operational SCBK was read back               */
    SC_KEY_UNREADABLE,  /* something is stored and it would not come back  */
} sc_key_state_t;

/* How the stored key is protected at rest — what to tell the operator, and
 * what the boot log's severity is chosen from. */
typedef enum {
    SC_KEY_PROT_PLAIN = 0,  /* no eFuse HMAC block; readable off the flash */
    SC_KEY_PROT_EFUSE,      /* wrapped under a read-protected eFuse key    */
} sc_key_prot_t;

/* Open the store and work out which protection is available.
 *
 * Requires nvs_flash_init() to have succeeded first. Does not read the key —
 * sc_key_load() does that — because the two questions have different answers
 * and the caller wants them separately: this one is about the device, that
 * one is about whether it has been keyed. */
esp_err_t sc_key_init(void);

/* Read the operational SCBK back, if there is one.
 *
 * On SC_KEY_LOADED, `scbk` holds the key. On anything else it is zeroed, so
 * a caller that ignores the return value cannot accidentally hand a stale
 * stack buffer to osdp_pd_set_sc_scbk. */
sc_key_state_t sc_key_load(uint8_t scbk[SC_KEY_LEN]);

/* Write a new operational SCBK, replacing whatever was there.
 *
 * Commits before returning: this is called from the osdp_KEYSET path, where
 * the ACU is about to be told the rotation succeeded, and a reply that
 * outruns the flash write is precisely the failure that leaves a reader
 * working until its next power cycle and unreachable after it. */
esp_err_t sc_key_store(const uint8_t scbk[SC_KEY_LEN]);

/* Erase the operational key, returning the reader to install mode.
 *
 * The only caller should be the physical reset in key_reset.h. There is
 * deliberately no command, no console verb and no remote path to this: a
 * reader that could be pushed back onto a published key by anything that
 * arrives over the wire would have no Secure Channel worth the name. */
esp_err_t sc_key_erase(void);

/* Which protection this device actually has. Valid after sc_key_init(). */
sc_key_prot_t sc_key_protection(void);

#endif /* SC_KEY_H */
