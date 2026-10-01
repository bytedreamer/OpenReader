#include "sc2_crypto.h"

#include "osdp_pair_wolfcrypt.h"   /* OSDP-Embedded ports/wolfcrypt */
#include "osdp_sc2_wolfcrypt.h"

/* Ports from 932e80c on define OSDP_SC2_WOLFCRYPT_HAS_KMAC, and when it is 1
 * the port binds KMAC256 from wolfSSL (5.9.4 and later, with WOLFSSL_KMAC).
 * Earlier ports don't define it at all, which #if reads as 0. */
#if !OSDP_SC2_WOLFCRYPT_HAS_KMAC
#include "kmac.h"                  /* OSDP-Embedded vendor/tiny-kmac */
#endif

#include "esp_log.h"
#include "esp_random.h"

#include <stdbool.h>

static const char *TAG = "sc2_crypto";

/* wolfCrypt state for each vtable. Static because the pairing one is several
 * KB, and both must outlive the PD they are bound to, which is forever. */
static osdp_sc2_wolfcrypt_t  s_sc2_ctx;
static osdp_pair_wolfcrypt_t s_pair_ctx;

static osdp_sc2_crypto_t  s_sc2;
static osdp_pair_crypto_t s_pair;

/* ---- Randomness ----------------------------------------------------------
 *
 * Both vtables draw from esp_fill_random, the same hardware RNG SC1 uses,
 * rather than from wolfCrypt's DRBG. The DRBG would need seeding from this
 * source anyway, and one source for every channel is one thing to get right.
 * It is a true RNG only while an entropy source is running. Nothing here
 * turns one on, because by the time sc2_bind() runs it already is:
 * pkoc_init() turns on the SAR-ADC source in a PKOC build, and
 * osdp_reader.c's bind_sc() does in one without PKOC.
 *
 * Pairing needs randomness even with a provisioned key: the ML-KEM
 * encapsulation, and the 32 fresh bytes in every hedged ML-DSA signature. */

static osdp_status_t sc2_rand(void *user, uint8_t *out, size_t len)
{
    (void)user;
    esp_fill_random(out, len);
    return OSDP_OK;
}

static osdp_status_t pair_rand(void *user, uint8_t *out, size_t len)
{
    (void)user;
    esp_fill_random(out, len);
    return OSDP_OK;
}

/* ---- KMAC256 -------------------------------------------------------------
 *
 * wolfSSL only gained KMAC in 5.9.4, and the ESP Component Registry stops at
 * 5.8.2, so on that build SC2's key derivation uses the library's vendored
 * tiny-kmac. Its header calls it test-only. That is acceptable on a bench,
 * and it has to be replaced before SC2 ships. */

#if !OSDP_SC2_WOLFCRYPT_HAS_KMAC
static osdp_status_t sc2_kmac256(void          *user,
                                 const uint8_t *key,  size_t key_len,
                                 const uint8_t *data, size_t data_len,
                                 uint8_t       *out,  size_t out_len)
{
    (void)user;
    tiny_kmac256(key, key_len, data, data_len, out, out_len);
    return OSDP_OK;
}
#endif

/* ---- Vtables -------------------------------------------------------------- */

const osdp_sc2_crypto_t *sc2_crypto_link(void)
{
    static bool ready;
    if (ready) {
        return &s_sc2;
    }

    /* The port fills in the AES-256 entries, `user`, and KMAC256 when its
     * wolfSSL has it. Randomness is ours, and so is KMAC256 otherwise. Ours
     * ignore `user`, so the port's context pointer there is left alone. */
    if (osdp_sc2_wolfcrypt_aes256(&s_sc2_ctx, &s_sc2) != OSDP_OK) {
        ESP_LOGE(TAG, "wolfCrypt AES-256 setup failed");
        return NULL;
    }
#if !OSDP_SC2_WOLFCRYPT_HAS_KMAC
    s_sc2.kmac256    = sc2_kmac256;
#endif
    s_sc2.rand_bytes = sc2_rand;

    ready = true;
    return &s_sc2;
}

const osdp_pair_crypto_t *sc2_crypto_pair(const uint8_t *dsa_pk,
                                          const uint8_t *dsa_sk,
                                          size_t         dsa_sk_len)
{
    static bool ready;
    if (ready) {
        return &s_pair;
    }

    if (osdp_pair_wolfcrypt_init(&s_pair_ctx, &s_pair) != OSDP_OK) {
        ESP_LOGE(TAG, "wolfCrypt pairing setup failed");
        return NULL;
    }
    osdp_pair_wolfcrypt_set_rand(&s_pair_ctx, pair_rand, NULL);

    if (dsa_sk_len != OSDP_PAIR_WOLFCRYPT_DSA_SK_LEN ||
        osdp_pair_wolfcrypt_set_dsa(&s_pair_ctx, dsa_pk, dsa_sk,
                                    dsa_sk_len) != OSDP_OK) {
        /* Most likely a pair_credentials.h from a different tool, or a
         * different ML-DSA parameter set. */
        ESP_LOGE(TAG, "the pairing private key was rejected (%u bytes, "
                      "expected %u)",
                 (unsigned)dsa_sk_len,
                 (unsigned)OSDP_PAIR_WOLFCRYPT_DSA_SK_LEN);
        osdp_pair_wolfcrypt_free(&s_pair_ctx);
        return NULL;
    }

    ready = true;
    return &s_pair;
}
