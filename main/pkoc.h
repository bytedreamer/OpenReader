/* PKOC — Public Key Open Credential — at the reader.
 *
 * The card holds a P-256 key pair generated inside its secure element. The
 * public key is the credential; it was registered with the access control
 * system at enrolment. At the door the card proves it holds the matching
 * private key by signing a nonce this reader supplies, and only then is the
 * credential derived from the key and sent to the ACU.
 *
 * That last ordering is the whole point. A public key read off a card and
 * reported without checking the signature is a value the card claimed —
 * replayable by anyone who has watched one exchange, and worth no more than
 * a UID. The AsymCred library enforces it (a result is unreachable until
 * the signature verifies) and this module does not offer a way around it.
 *
 * The protocol work is AsymCred's, in the sibling repository; the card
 * transport is rc522.c's ISO-DEP layer. What lives here is the join: the
 * crypto primitives this board can supply, the reader's own identity, and
 * one function that turns a card sitting on the antenna into a credential.
 */
#ifndef PKOC_H
#define PKOC_H

#include "credential.h"
#include "esp_err.h"

/* Outcomes that are specifically PKOC's, kept out of the esp_err_t space
 * everything else uses because the caller has to tell them apart.
 *
 * The distinction that matters is NO_APPLET against BAD_SIGNATURE. The first
 * means this is an ordinary card and reporting its UID instead is the right
 * thing. The second means a card claimed a credential it could not prove it
 * owned, and must never fall back to anything. */
#define PKOC_ERR_BASE          0x9000
#define PKOC_ERR_NO_APPLET     (PKOC_ERR_BASE + 1)  /* no PKOC on this card */
#define PKOC_ERR_BAD_SIGNATURE (PKOC_ERR_BASE + 2)  /* it did not verify    */
#define PKOC_ERR_TRANSACTION   (PKOC_ERR_BASE + 3)  /* exchange went wrong  */

/* Bind the crypto primitives and read the reader identity out of Kconfig.
 * Call once, before any card is polled. Failure means the board cannot
 * supply something PKOC requires — an entropy source, most likely — and the
 * caller should carry on reading UIDs rather than not come up at all. */
esp_err_t pkoc_init(void);

/* Run one PKOC transaction against the card rc522_poll() just selected, and
 * derive the configured credential from the verified public key.
 *
 * Must be called with a card still selected and before rc522_release(). The
 * caller should check rc522_uid_is_iso14443_4() first: a card that does not
 * speak ISO-DEP costs a RATS timeout to discover here.
 *
 *   ESP_OK                  `out` holds a verified credential
 *   PKOC_ERR_NO_APPLET      an ordinary card; report its UID instead
 *   PKOC_ERR_BAD_SIGNATURE  report nothing, and say so loudly
 *   PKOC_ERR_TRANSACTION    the exchange failed; usually a card withdrawn
 *                           mid-transaction, which is not worth a warning
 */
esp_err_t pkoc_read(credential_t *out);

#endif /* PKOC_H */
