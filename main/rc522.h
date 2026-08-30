/* Minimal MFRC522 driver — enough to detect an ISO/IEC 14443-A card and
 * read its UID. No MIFARE Classic authentication or block I/O: an OSDP
 * reader reports the credential it saw and lets the ACU decide, so the UID
 * is the whole job.
 */
#ifndef RC522_H
#define RC522_H

#include "esp_err.h"
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
 * Cheap enough to call a few times a second.
 */
esp_err_t rc522_poll(rc522_uid_t *out);

#endif /* RC522_H */
