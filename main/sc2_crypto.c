#include "sc2_crypto.h"

/* Placeholder until OSDP-Embedded's ports/wolfcrypt exists. Both getters
 * report "no backend", which sc2.c turns into SC2 staying off with a log
 * line, so the rest of the SC2 wiring can be built and exercised before the
 * crypto is.
 *
 * When the port lands, this file binds it: the port's vtables for AES-GCM,
 * the AES block, ML-KEM, ML-DSA and the hashes, tiny-kmac for KMAC256, and
 * esp_fill_random for randomness. The SAR-ADC entropy source that makes
 * esp_fill_random truly random is already on by the time sc2_bind() runs:
 * pkoc_init() turns it on in a PKOC build, and osdp_reader.c's bind_sc()
 * does in one without PKOC. */

const osdp_sc2_crypto_t *sc2_crypto_link(void)
{
    return NULL;
}

const osdp_pair_crypto_t *sc2_crypto_pair(const uint8_t *dsa_pk,
                                          const uint8_t *dsa_sk,
                                          size_t         dsa_sk_len)
{
    (void)dsa_pk;
    (void)dsa_sk;
    (void)dsa_sk_len;
    return NULL;
}
