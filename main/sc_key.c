#include "sc_key.h"

#include "osdp/osdp_sc.h"       /* OSDP_SC_KEY_LEN — checked against ours */

#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "sc_key";

/* Its own namespace rather than a key in a shared one. The SCBK is the only
 * secret this firmware holds, and keeping it in a namespace nothing else
 * opens means nvs_erase_all() somewhere else can never take it out from
 * under the reader. */
#define NVS_NAMESPACE "openreader_sc"
#define NVS_KEY_SCBK  "scbk"

/* The two blob shapes. The first byte says which, so a device whose eFuse
 * key was burned after deployment can still read what it wrote before. */
#define BLOB_PLAIN 0x01U   /* [kind][key 16]                               */
#define BLOB_EFUSE 0x02U   /* [kind][nonce 12][tag 16][ciphertext 16]      */

#define GCM_NONCE_LEN 12U
#define GCM_TAG_LEN   16U
#define KEK_LEN       32U   /* HMAC-SHA256 output, used as an AES-256 key  */

#define BLOB_PLAIN_LEN (1U + SC_KEY_LEN)
#define BLOB_EFUSE_LEN (1U + GCM_NONCE_LEN + GCM_TAG_LEN + SC_KEY_LEN)

/* Domain separation for the KEK derivation.
 *
 * The eFuse block is a general-purpose HMAC key that other subsystems could
 * legitimately want to use — JTAG re-enablement and the Digital Signature
 * peripheral both consume one. Deriving through a fixed, purpose-naming
 * string means this module's wrapping key cannot collide with anything
 * else's, and the version in it means a future change of scheme does not
 * silently produce the same KEK for a different format. Change the string
 * and every previously stored blob stops opening — which is why it is a
 * constant here and not a configurable. */
static const char KEK_LABEL[] = "OpenReader SCBK wrap v1";

_Static_assert(SC_KEY_LEN == OSDP_SC_KEY_LEN,
               "sc_key.h's SC_KEY_LEN has drifted from the library's "
               "OSDP_SC_KEY_LEN");

static bool              s_ready;
static bool              s_have_efuse;
static esp_efuse_block_t s_efuse_block;

/* ---- The wrapping key --------------------------------------------------- */

/* Derive the key-encryption key from the eFuse block.
 *
 * esp_hmac_calculate drives the HMAC peripheral in "upstream" mode: the
 * hardware reads the eFuse key internally and returns only the MAC. The key
 * itself never appears on a bus this CPU can see, which is the entire reason
 * this is worth doing rather than keeping a wrapping key in the image.
 *
 * The KEK is a stack buffer in every caller and is wiped there. It has a
 * lifetime of one AES-GCM operation by design — there is no cached copy for
 * an attacker with a memory dump to find, and re-deriving it costs a few
 * microseconds of a peripheral that exists for this. */
static esp_err_t derive_kek(uint8_t kek[KEK_LEN])
{
    if (!s_have_efuse) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* The label's terminating NUL is deliberately excluded: what is hashed
     * is the 23 characters, not a C implementation detail. */
    return esp_hmac_calculate((hmac_key_id_t)(s_efuse_block - EFUSE_BLK_KEY0),
                              KEK_LABEL, sizeof(KEK_LABEL) - 1U, kek);
}

/* AES-256-GCM in one direction or the other. Split out because wrap and
 * unwrap differ in three lines and share every opportunity to get the
 * lengths wrong.
 *
 * Both authenticate the blob's kind byte as additional data. It is not
 * secret and it is already stored in the clear, so authenticating it costs
 * nothing and buys the guarantee that a blob relabelled from BLOB_EFUSE to
 * BLOB_PLAIN — or the reverse — fails its tag rather than being opened as
 * something it is not. */
static esp_err_t gcm_seal(const uint8_t kek[KEK_LEN],
                          const uint8_t nonce[GCM_NONCE_LEN],
                          const uint8_t *plain, uint8_t *cipher,
                          uint8_t tag[GCM_TAG_LEN])
{
    const uint8_t aad = BLOB_EFUSE;
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    esp_err_t err = ESP_FAIL;
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, kek,
                           KEK_LEN * 8U) == 0 &&
        mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, SC_KEY_LEN,
                                  nonce, GCM_NONCE_LEN, &aad, 1U,
                                  plain, cipher, GCM_TAG_LEN, tag) == 0) {
        err = ESP_OK;
    }
    mbedtls_gcm_free(&ctx);
    return err;
}

static esp_err_t gcm_open(const uint8_t kek[KEK_LEN],
                          const uint8_t nonce[GCM_NONCE_LEN],
                          const uint8_t tag[GCM_TAG_LEN],
                          const uint8_t *cipher, uint8_t *plain)
{
    const uint8_t aad = BLOB_EFUSE;
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    /* auth_decrypt, not decrypt-then-compare: it checks the tag in constant
     * time and writes nothing when the check fails, so a corrupted blob
     * cannot leave a half-decrypted key in the caller's buffer. */
    esp_err_t err = ESP_FAIL;
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, kek,
                           KEK_LEN * 8U) == 0 &&
        mbedtls_gcm_auth_decrypt(&ctx, SC_KEY_LEN, nonce, GCM_NONCE_LEN,
                                 &aad, 1U, tag, GCM_TAG_LEN,
                                 cipher, plain) == 0) {
        err = ESP_OK;
    }
    mbedtls_gcm_free(&ctx);
    return err;
}

/* ---- Blob assembly ------------------------------------------------------ */

/* Build the blob that goes into NVS, in whichever shape this device can
 * manage. `out` must hold BLOB_EFUSE_LEN; `*len` receives what was used. */
static esp_err_t blob_seal(const uint8_t scbk[SC_KEY_LEN],
                           uint8_t *out, size_t *len)
{
    if (!s_have_efuse) {
        out[0] = BLOB_PLAIN;
        memcpy(&out[1], scbk, SC_KEY_LEN);
        *len = BLOB_PLAIN_LEN;
        return ESP_OK;
    }

    uint8_t kek[KEK_LEN];
    esp_err_t err = derive_kek(kek);
    if (err != ESP_OK) {
        return err;
    }

    out[0] = BLOB_EFUSE;
    uint8_t *nonce  = &out[1];
    uint8_t *tag    = &out[1 + GCM_NONCE_LEN];
    uint8_t *cipher = &out[1 + GCM_NONCE_LEN + GCM_TAG_LEN];

    /* A fresh nonce on every write. GCM's failure mode for a repeated
     * nonce under the same key is catastrophic — two ciphertexts under one
     * (key, nonce) leak their XOR and forge-enable the whole stream — and
     * the KEK here never changes, so the nonce is the only thing standing
     * between two rotations of the same reader's key. esp_fill_random is
     * the hardware RNG. */
    esp_fill_random(nonce, GCM_NONCE_LEN);

    err = gcm_seal(kek, nonce, scbk, cipher, tag);
    memset(kek, 0, sizeof(kek));
    if (err != ESP_OK) {
        memset(out, 0, BLOB_EFUSE_LEN);
        return err;
    }
    *len = BLOB_EFUSE_LEN;
    return ESP_OK;
}

/* The inverse. Returns ESP_ERR_INVALID_STATE for a blob this device cannot
 * open — which the caller must report as a fault, never as "unkeyed". */
static esp_err_t blob_open(const uint8_t *blob, size_t len,
                           uint8_t scbk[SC_KEY_LEN])
{
    if (len == BLOB_PLAIN_LEN && blob[0] == BLOB_PLAIN) {
        memcpy(scbk, &blob[1], SC_KEY_LEN);
        return ESP_OK;
    }
    if (len != BLOB_EFUSE_LEN || blob[0] != BLOB_EFUSE) {
        ESP_LOGE(TAG, "stored key blob is malformed (%u bytes, kind 0x%02X)",
                 (unsigned)len, (unsigned)blob[0]);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t kek[KEK_LEN];
    if (derive_kek(kek) != ESP_OK) {
        /* A wrapped blob on a device with no HMAC eFuse block. Either the
         * flash has been moved to a different chip, or this one was
         * provisioned and then somehow reverted — the second is impossible,
         * eFuses do not un-burn, which makes the first the answer. */
        ESP_LOGE(TAG, "the stored key is wrapped to an eFuse this chip does "
                      "not have — flash from a different device?");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = gcm_open(kek, &blob[1], &blob[1 + GCM_NONCE_LEN],
                                   &blob[1 + GCM_NONCE_LEN + GCM_TAG_LEN],
                                   scbk);
    memset(kek, 0, sizeof(kek));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the stored key failed its authentication tag — the "
                      "blob is corrupt or was written by another device");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t write_blob(const uint8_t *blob, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_SCBK, blob, len);
    if (err == ESP_OK) {
        /* Commit inside the same handle, before returning success. The
         * caller's next act is to tell the ACU the key took. */
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- Public ------------------------------------------------------------- */

esp_err_t sc_key_init(void)
{
    /* Which eFuse block, if any, has been burned for HMAC-upstream use.
     * Discovered rather than configured: the block number is a fact about
     * the chip in front of us, and a build-time constant that disagreed
     * with it would fail as an unreadable key rather than as a mismatch. */
    s_have_efuse = esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP,
                                          &s_efuse_block);
    s_ready = true;

    if (s_have_efuse) {
        ESP_LOGI(TAG, "SCBK store: wrapped under eFuse BLOCK_KEY%d "
                      "(HMAC upstream)",
                 (int)(s_efuse_block - EFUSE_BLK_KEY0));
    } else {
        /* ESP_LOGE, every boot, unconditionally. This is the one condition
         * in which the reader's only secret is sitting in readable flash,
         * and a warning that scrolls past once is not proportionate to
         * that. */
        ESP_LOGE(TAG, "SCBK store: NO eFuse HMAC key burned — the Secure "
                      "Channel key is stored in the clear and can be read "
                      "off this board with esptool. See sc_key.h.");
    }
    return ESP_OK;
}

sc_key_state_t sc_key_load(uint8_t scbk[SC_KEY_LEN])
{
    memset(scbk, 0, SC_KEY_LEN);
    if (!s_ready) {
        return SC_KEY_UNREADABLE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return SC_KEY_NONE;   /* the namespace has never been written */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not open the key store: %s",
                 esp_err_to_name(err));
        return SC_KEY_UNREADABLE;
    }

    uint8_t blob[BLOB_EFUSE_LEN];
    size_t  len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY_SCBK, blob, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return SC_KEY_NONE;   /* never keyed — install mode */
    }
    if (err != ESP_OK) {
        /* Includes ESP_ERR_NVS_INVALID_LENGTH, which means something is
         * stored that is larger than any blob this firmware writes. Stored
         * and unreadable is a fault, not an absence. */
        ESP_LOGE(TAG, "could not read the stored key: %s",
                 esp_err_to_name(err));
        return SC_KEY_UNREADABLE;
    }

    if (blob_open(blob, len, scbk) != ESP_OK) {
        memset(scbk, 0, SC_KEY_LEN);
        return SC_KEY_UNREADABLE;
    }

    /* Upgrade in place: a key stored in the clear on a device that has since
     * had its eFuse block burned gets rewrapped now. Burning the eFuse after
     * deployment is otherwise a promise that only applies to the next key
     * rotation, which is not what anyone doing it believes they are getting.
     *
     * A failure here is not fatal — the key is in hand and the reader should
     * still come up — so it is reported and stepped over. */
    if (blob[0] == BLOB_PLAIN && s_have_efuse) {
        ESP_LOGW(TAG, "rewrapping the stored key under the eFuse HMAC block");
        if (sc_key_store(scbk) != ESP_OK) {
            ESP_LOGE(TAG, "rewrap failed; the key remains stored in the clear");
        }
    }

    memset(blob, 0, sizeof(blob));
    return SC_KEY_LOADED;
}

esp_err_t sc_key_store(const uint8_t scbk[SC_KEY_LEN])
{
    if (!s_ready || scbk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t blob[BLOB_EFUSE_LEN];
    size_t  len = 0;

    esp_err_t err = blob_seal(scbk, blob, &len);
    if (err == ESP_OK) {
        err = write_blob(blob, len);
    }
    memset(blob, 0, sizeof(blob));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing the new SCBK failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Deliberately says nothing about the key itself, not even its length —
     * a log line is the easiest place in a system for a secret to end up. */
    ESP_LOGI(TAG, "new SCBK stored (%s)",
             s_have_efuse ? "eFuse-wrapped" : "IN THE CLEAR");
    return ESP_OK;
}

esp_err_t sc_key_erase(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* nothing stored; already in install mode */
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(h, NVS_KEY_SCBK);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "SCBK erased — this reader is back in install mode "
                      "and will answer a handshake on SCBK-D");
    }
    return err;
}

sc_key_prot_t sc_key_protection(void)
{
    return s_have_efuse ? SC_KEY_PROT_EFUSE : SC_KEY_PROT_PLAIN;
}
