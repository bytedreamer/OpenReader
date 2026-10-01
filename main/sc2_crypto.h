/* The crypto behind SC2 and pairing, in one place.
 *
 * The library defines what it needs as two vtables and leaves filling them
 * in to the application: osdp_sc2_crypto_t for the link (KMAC256,
 * AES-256-GCM, the AES-256 block, randomness) and osdp_pair_crypto_t for
 * pairing (ML-KEM-768, ML-DSA-44, SHA-256, HMAC, HKDF, randomness). This
 * header is the whole of OpenReader's view of them. sc2.c asks for the two
 * vtables and never names a crypto library, so changing the backend is
 * this file's .c and nothing else.
 *
 * The backend is wolfCrypt, through OSDP-Embedded's ports/wolfcrypt, plus
 * the library's vendored tiny-kmac for KMAC256, which wolfCrypt lacks.
 * Both getters set their vtable up on the first call and return the same
 * one after that. */
#ifndef SC2_CRYPTO_H
#define SC2_CRYPTO_H

#include "osdp/osdp_pair_crypto.h"
#include "osdp/osdp_sc2_crypto.h"

#include <stddef.h>
#include <stdint.h>

/* The SC2 link crypto, or NULL if this build has no backend for it. */
const osdp_sc2_crypto_t *sc2_crypto_link(void);

/* The pairing crypto, holding this PD's ML-DSA-44 signing key. `dsa_pk` is
 * OSDP_MLDSA44_PK_LEN bytes; `dsa_sk` is the private key as
 * osdp-pair-provision wrote it. NULL if this build has no backend, or if the
 * backend rejects the key. */
const osdp_pair_crypto_t *sc2_crypto_pair(const uint8_t *dsa_pk,
                                          const uint8_t *dsa_sk,
                                          size_t         dsa_sk_len);

#endif /* SC2_CRYPTO_H */
