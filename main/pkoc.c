#include "pkoc.h"
#include "rc522.h"

#include "asymcred/asymcred_pkoc.h"
#include "asymcred/asymcred_apdu.h"

#include "bootloader_random.h"   /* entropy when the radio is not running */
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "pkoc";

/* ---- Crypto ------------------------------------------------------------
 *
 * AsymCred vendors no crypto: it asks for SHA-256, an ECDSA P-256
 * verification and a CSPRNG, and the consumer binds whatever the deployment
 * already has. On this board that is mbedTLS, which ESP-IDF ships and which
 * uses the C6's own accelerators where it can.
 *
 * Verification operates only on public values — the key the card sent and
 * the signature over a nonce we chose — so unlike the card's signing half it
 * is not side-channel sensitive. The random path is the one that matters.
 */

static asymcred_status_t hal_sha256(void *user, const uint8_t *msg, size_t len,
                                    uint8_t out[ASYMCRED_SHA256_LEN])
{
    (void)user;
    return (mbedtls_sha256(msg, len, out, 0) == 0) ? ASYMCRED_OK
                                                   : ASYMCRED_ERR_NOT_SUPPORTED;
}

static asymcred_status_t hal_p256_verify(void *user,
                                         const uint8_t pub[ASYMCRED_P256_PUBKEY_LEN],
                                         const uint8_t digest[ASYMCRED_SHA256_LEN],
                                         const uint8_t sig[ASYMCRED_P256_SIG_LEN])
{
    (void)user;

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi       r;
    mbedtls_mpi       s;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    /* Anything short of a valid signature over a valid point is the same
     * answer to the caller — the credential is refused — so the failures
     * funnel to one status rather than being classified. The distinction
     * that matters is between this and a plumbing error, and there is no
     * plumbing error here that is not also a refusal. */
    asymcred_status_t st = ASYMCRED_ERR_BAD_SIGNATURE;

    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&grp, &Q, pub,
                                      ASYMCRED_P256_PUBKEY_LEN) == 0 &&
        /* The card sent this point; nothing has checked it is on the curve.
         * mbedtls_ecdsa_verify would reject an off-curve point anyway, but
         * saying so explicitly is what the HAL contract asks for. */
        mbedtls_ecp_check_pubkey(&grp, &Q) == 0 &&
        mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
        mbedtls_mpi_read_binary(&s, &sig[32], 32) == 0 &&
        mbedtls_ecdsa_verify(&grp, digest, ASYMCRED_SHA256_LEN,
                             &Q, &r, &s) == 0) {
        st = ASYMCRED_OK;
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return st;
}

static asymcred_status_t hal_rand(void *user, uint8_t *out, size_t len)
{
    (void)user;
    esp_fill_random(out, len);
    return ASYMCRED_OK;
}

static const asymcred_crypto_t s_crypto = {
    .sha256            = hal_sha256,
    .ecdsa_p256_verify = hal_p256_verify,
    .rand_bytes        = hal_rand,
    .user              = NULL,
};

/* ---- Reader identity ---------------------------------------------------- */

/* The 32 bytes AUTHENTICATE carries: a 16-byte site key identifier followed
 * by a 16-byte reader location identifier. Both come from Kconfig as hex.
 *
 * The card is not required to do anything with them and the Z-bit applet
 * ignores them, so an all-zero default is honest rather than lazy: it says
 * this reader has not been assigned a site. Fill them in when the access
 * control system issues you one. */
static asymcred_pkoc_config_t s_config;

/* Versions this reader can actually speak, most preferred first. Being
 * explicit rather than passing NULL matters: NULL means "take whatever the
 * card lists first", which would have this reader agree to a version of the
 * protocol it does not implement. */
static const uint16_t kVersions[] = {
    ASYMCRED_PKOC_VERSION_1_1,
    ASYMCRED_PKOC_VERSION_1_0,
};

#if CONFIG_OPENREADER_PKOC_CRED_256
#define CRED_SIZE  ASYMCRED_PKOC_CRED_256BIT
#define CRED_WIDTH "256"
#elif CONFIG_OPENREADER_PKOC_CRED_75
#define CRED_SIZE  ASYMCRED_PKOC_CRED_75BIT
#define CRED_WIDTH "75"
#else
#define CRED_SIZE  ASYMCRED_PKOC_CRED_64BIT
#define CRED_WIDTH "64"
#endif

/* Parse exactly `bytes` bytes of hex. Returns false on anything malformed,
 * which the caller turns into a warning and a zero-filled field — a reader
 * that refuses to come up over a typo in an identifier the card ignores
 * would be trading a working door for a cosmetic fault. */
static bool parse_hex(const char *hex, uint8_t *out, size_t bytes)
{
    if (hex == NULL || strlen(hex) != bytes * 2U) {
        return false;
    }
    for (size_t i = 0U; i < bytes; i++) {
        uint8_t v = 0U;
        for (int half = 0; half < 2; half++) {
            const char c = hex[i * 2U + (size_t)half];
            uint8_t    n;
            if (c >= '0' && c <= '9')      { n = (uint8_t)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { n = (uint8_t)(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { n = (uint8_t)(c - 'A' + 10); }
            else                           { return false; }
            v = (uint8_t)((v << 4) | n);
        }
        out[i] = v;
    }
    return true;
}

esp_err_t pkoc_init(void)
{
    memset(&s_config, 0, sizeof(s_config));

    uint8_t site[ASYMCRED_PKOC_SITE_ID_LEN];
    uint8_t location[ASYMCRED_PKOC_LOCATION_ID_LEN];

    if (!parse_hex(CONFIG_OPENREADER_PKOC_SITE_ID, site, sizeof(site))) {
        ESP_LOGW(TAG, "site identifier is not %u hex characters; sending "
                      "zeros", (unsigned)(sizeof(site) * 2U));
        memset(site, 0, sizeof(site));
    }
    if (!parse_hex(CONFIG_OPENREADER_PKOC_LOCATION_ID, location,
                   sizeof(location))) {
        ESP_LOGW(TAG, "location identifier is not %u hex characters; sending "
                      "zeros", (unsigned)(sizeof(location) * 2U));
        memset(location, 0, sizeof(location));
    }
    (void)asymcred_pkoc_make_reader_id(site, location, s_config.reader_id);

    s_config.supported_versions      = kVersions;
    s_config.supported_version_count = sizeof(kVersions) / sizeof(kVersions[0]);

    /* Spelled out rather than assigned from the symbol: Kconfig does not
     * define a bool that is off, so the obvious assignment compiles only in
     * the configuration where it does not matter. */
#ifdef CONFIG_OPENREADER_PKOC_REQUIRE_SIGNATURE
    s_config.require_signature = true;
#else
    s_config.require_signature = false;
#endif

    /* The transaction identifier is the only thing making the card's
     * signature fresh, so where it comes from is the security of the whole
     * scheme rather than a detail.
     *
     * esp_random() is a true random number generator only while the RF
     * subsystem is running, and this firmware never brings up Wi-Fi or
     * Bluetooth. Without an entropy source it would return a repeatable
     * sequence — and a repeatable nonce makes a captured card response
     * replayable, which is precisely what PKOC exists to prevent. This
     * enables the SAR-ADC entropy source instead, which is what ESP-IDF
     * provides for exactly this case. Nothing in this firmware uses the ADC,
     * so it can stay on for the life of the device. */
    bootloader_random_enable();

    if (!s_config.require_signature) {
        ESP_LOGW(TAG, "SIGNATURE VERIFICATION IS OFF. Every public key a "
                      "card offers will be reported to the ACU unchecked, "
                      "including a replayed one. Enrolment and bench use "
                      "only — never a door.");
    }

    ESP_LOGI(TAG, "PKOC ready: %s-bit credentials, signature %s",
             CRED_WIDTH,
             s_config.require_signature ? "verified" : "NOT VERIFIED");
    return ESP_OK;
}

/* ---- The transaction ---------------------------------------------------- */

/* AsymCred is a step machine and does no I/O of its own, so this is the
 * blocking transceive it wraps: hand the C-APDU to the ISO-DEP layer, hand
 * the R-APDU back. */
static asymcred_status_t transceive(void *user,
                                    const uint8_t *cmd, size_t cmd_len,
                                    uint8_t *rsp, size_t rsp_cap,
                                    size_t *rsp_len)
{
    (void)user;

    const esp_err_t err = rc522_iso_dep_transceive(cmd, cmd_len, rsp, rsp_cap,
                                                   rsp_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return ASYMCRED_ERR_BUFFER_TOO_SMALL;
    }
    return (err == ESP_OK) ? ASYMCRED_OK : ASYMCRED_ERR_TRUNCATED;
}

esp_err_t pkoc_read(credential_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = rc522_iso_dep_activate();
    if (err != ESP_OK) {
        /* A card that never answers RATS is an ordinary badge, not a fault.
         * Same treatment as an applet that is not there. */
        ESP_LOGD(TAG, "no ISO-DEP: %s", esp_err_to_name(err));
        return PKOC_ERR_NO_APPLET;
    }

    asymcred_pkoc_t        pkoc;
    asymcred_pkoc_result_t res;

    asymcred_status_t st = asymcred_pkoc_begin(&pkoc, &s_config, &s_crypto);
    if (st != ASYMCRED_OK) {
        ESP_LOGE(TAG, "could not start a transaction (%d)", (int)st);
        return PKOC_ERR_TRANSACTION;
    }

    st = asymcred_pkoc_run(&pkoc, transceive, NULL, &res);
    if (st != ASYMCRED_OK) {
        const uint16_t sw = asymcred_pkoc_card_sw(&pkoc);

        switch (st) {
        case ASYMCRED_ERR_NO_APPLET:
            /* SELECT was refused: a smart card, but not a PKOC one. */
            ESP_LOGD(TAG, "no PKOC applet (SW %04X)", sw);
            return PKOC_ERR_NO_APPLET;

        case ASYMCRED_ERR_BAD_SIGNATURE:
            /* The exchange completed and the maths did not. This is the one
             * failure that is about the credential rather than the link, and
             * the only one that must never fall back to reporting a UID: a
             * card that cannot prove it holds the private key has told us
             * nothing, and reporting the UID it also carries would hand the
             * ACU something to make a decision on. */
            ESP_LOGW(TAG, "PKOC signature did NOT verify — credential "
                          "refused");
            return PKOC_ERR_BAD_SIGNATURE;

        case ASYMCRED_ERR_VERSION:
            ESP_LOGW(TAG, "no PKOC version in common with this card");
            return PKOC_ERR_TRANSACTION;

        default:
            /* Overwhelmingly a card pulled off the antenna partway through.
             * A PKOC exchange is several round trips and a signature, so it
             * is long enough to be interrupted by an ordinary human hand. */
            ESP_LOGD(TAG, "transaction failed (%d, SW %04X)", (int)st, sw);
            return PKOC_ERR_TRANSACTION;
        }
    }

    memset(out, 0, sizeof(*out));

    size_t written = 0U;
    size_t bits    = 0U;
    st = asymcred_pkoc_credential(res.public_key, CRED_SIZE, out->bytes,
                                  sizeof(out->bytes), &written, &bits);
    if (st != ASYMCRED_OK) {
        ESP_LOGE(TAG, "credential derivation failed (%d)", (int)st);
        return PKOC_ERR_TRANSACTION;
    }

    out->kind      = CREDENTIAL_PKOC;
    out->len       = (uint8_t)written;
    out->bit_count = (uint16_t)bits;
    out->verified  = res.signature_verified;

    ESP_LOGI(TAG, "PKOC v%u.%u verified, %u-bit credential",
             (unsigned)(res.version >> 8), (unsigned)(res.version & 0xFFU),
             (unsigned)bits);
    return ESP_OK;
}
