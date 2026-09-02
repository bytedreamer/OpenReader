/* What this reader read, in the one shape everything downstream wants.
 *
 * The reader used to have a single kind of credential — the 14443-3 UID —
 * and passed an rc522_uid_t straight from the card task to the OSDP task
 * and to the panel. PKOC ends that: the value the ACU is told is derived
 * from a public key, is up to 32 bytes wide, and carries a bit count that
 * is not simply eight times its length. So the card task now produces a
 * credential and nobody downstream needs to know which card standard
 * produced it — except the display, which says so, because a person
 * standing at the door is the one party who benefits from knowing.
 */
#ifndef CREDENTIAL_H
#define CREDENTIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The widest thing this reader produces: a full 256-bit PKOC credential.
 * A UID is at most 10 bytes. */
#define CREDENTIAL_MAX_BYTES 32

typedef enum {
    CREDENTIAL_UID = 0,   /* the ISO/IEC 14443-3 UID, off any card       */
    CREDENTIAL_PKOC,      /* derived from a PKOC public key              */
} credential_kind_t;

typedef struct {
    uint8_t  bytes[CREDENTIAL_MAX_BYTES];
    uint8_t  len;
    /* What goes in the osdp_RAW header. Not always 8 * len: the 75-bit PKOC
     * form occupies ten bytes whose top five bits are zero. */
    uint16_t bit_count;
    uint8_t  kind;        /* credential_kind_t */
    /* Whether the card proved it holds the private key. Always false for a
     * UID, which proves nothing by construction. Carried rather than assumed
     * because a build with signature checking turned off still produces PKOC
     * credentials, and nothing downstream should render those identically to
     * a real one. */
    bool     verified;
} credential_t;

/* Uppercase hex, no separators. Writes at most `cap` bytes including the
 * terminator and returns the number of characters written. */
size_t credential_hex(const credential_t *c, char *buf, size_t cap);

/* "UID" or "PKOC", for a log line or a panel label. */
const char *credential_kind_name(const credential_t *c);

/* True when both describe the same read — same kind, same bits. Used to
 * suppress a card left sitting on the antenna. */
bool credential_equal(const credential_t *a, const credential_t *b);

#endif /* CREDENTIAL_H */
