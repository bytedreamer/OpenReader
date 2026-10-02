#include "sc2_crypto.h"

#include "osdp_pair_wolfcrypt.h"   /* OSDP-Embedded ports/wolfcrypt */
#include "osdp_sc2_wolfcrypt.h"

/* KMAC256 comes from wolfCrypt too, through the port. That needs wolfSSL
 * 5.9.4 or later with WOLFSSL_KMAC (see components/wolfssl), and an
 * OSDP-Embedded new enough to bind it (932e80c). An older port doesn't
 * define the flag at all, which #if reads as 0. */
#if !OSDP_SC2_WOLFCRYPT_HAS_KMAC
#error "The wolfCrypt SC2 port has no KMAC256: wolfSSL must be 5.9.4+ with WOLFSSL_KMAC, and OSDP-Embedded at 932e80c or later"
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

/* ---- Vtables -------------------------------------------------------------- */

const osdp_sc2_crypto_t *sc2_crypto_link(void)
{
    static bool ready;
    if (ready) {
        return &s_sc2;
    }

    /* The port fills in AES-256-GCM, the AES-256 block, KMAC256 and `user`.
     * Randomness is ours, and ignores `user`, so the port's context pointer
     * there is left alone. */
    if (osdp_sc2_wolfcrypt_aes256(&s_sc2_ctx, &s_sc2) != OSDP_OK) {
        ESP_LOGE(TAG, "wolfCrypt AES-256 setup failed");
        return NULL;
    }
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
