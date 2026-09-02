/* Minimal MFRC522 driver.
 *
 * Two layers, because this reader now has two kinds of credential to read.
 *
 * ISO/IEC 14443-3 — anticollision and SELECT, which yields the card's UID.
 * That is the whole transaction for a MIFARE-style badge: an OSDP reader
 * reports the credential it saw and lets the ACU decide, so there is no
 * MIFARE Classic authentication or block I/O here.
 *
 * ISO/IEC 14443-4 (ISO-DEP, "T=CL") — the APDU transport a smart card
 * speaks, layered on top of the same activation. PKOC needs it: SELECT the
 * applet, AUTHENTICATE, read back a 65-byte public key and a 64-byte
 * signature. Everything about that is bigger than the MFRC522's 64-byte
 * FIFO, which is exactly what ISO-DEP's chaining is for.
 *
 * Ownership. rc522_poll() leaves the card selected and powered — it has to,
 * or there would be nothing left to run an APDU exchange against. The caller
 * must therefore call rc522_release() when it is finished with the card,
 * whether or not it went on to use ISO-DEP. Failing to would leave the card
 * responding to the next poll as an already-selected card and the
 * anticollision would not run.
 */
#ifndef RC522_H
#define RC522_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* A UID is 4, 7 or 10 bytes depending on how many cascade levels the card
 * needs (single, double, triple). */
#define RC522_UID_MAX_BYTES 10

typedef struct {
    uint8_t bytes[RC522_UID_MAX_BYTES];
    uint8_t len;
    uint8_t sak;   /* Select AcKnowledge from the final cascade level */
} rc522_uid_t;

/* SAK bit 5 is the card's own statement that it speaks ISO/IEC 14443-4.
 * Asked as a question about a UID rather than tested inline so the reason
 * for the bit is written down once. */
static inline bool rc522_uid_is_iso14443_4(const rc522_uid_t *uid)
{
    return uid != NULL && (uid->sak & 0x20U) != 0U;
}

/* Add the RC522 to the already-initialised shared SPI bus and reset it into
 * a known state with its antenna on. Returns ESP_ERR_NOT_FOUND if the chip
 * does not answer with a plausible VersionReg — almost always wiring. */
esp_err_t rc522_init(void);

/* Poll once for a card in the field.
 *
 *   ESP_OK              a card answered and `out` holds its UID
 *   ESP_ERR_NOT_FOUND   no card in the field (the common case; not an error)
 *   anything else       a transport or protocol failure worth logging
 *
 * On ESP_OK the card is left selected. Call rc522_release() when done.
 *
 * Cheap enough to call a few times a second.
 */
esp_err_t rc522_poll(rc522_uid_t *out);

/* Let the card go: S(DESELECT) when ISO-DEP was activated, HLTA otherwise.
 * Either way the card stops answering REQA, so a badge left on the antenna
 * does not re-read on every poll. Safe to call when no card is selected. */
void rc522_release(void);

/* ---- ISO/IEC 14443-4 (ISO-DEP) ------------------------------------------
 *
 * Only for a card whose SAK says it speaks the protocol — see
 * rc522_uid_is_iso14443_4(). Activating one that does not simply times out.
 */

/* Send RATS and adopt the card's ATS: frame size, frame waiting time, and
 * the guard time before the first APDU. Must follow a successful
 * rc522_poll() with no other exchange in between.
 *
 *   ESP_ERR_NOT_FOUND       nothing answered RATS. A card with no ISO-DEP
 *                           in it stays silent rather than refusing, so
 *                           this also covers "not a smart card after all"
 *   ESP_ERR_NOT_SUPPORTED   something answered, but not with a usable ATS
 */
esp_err_t rc522_iso_dep_activate(void);

/* Exchange one APDU. Chaining is handled in both directions and an S(WTX)
 * from the card extends the wait rather than failing, so the caller sees a
 * whole C-APDU in and a whole R-APDU out however the card chose to frame it.
 *
 * `rx_len` receives the response length including its two status bytes.
 * ESP_ERR_INVALID_SIZE means the response did not fit `rx_cap`. */
esp_err_t rc522_iso_dep_transceive(const uint8_t *tx, size_t tx_len,
                                   uint8_t *rx, size_t rx_cap,
                                   size_t *rx_len);

#endif /* RC522_H */
