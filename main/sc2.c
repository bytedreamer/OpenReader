#include "sc2.h"
#include "sc2_crypto.h"
#include "sc_key.h"

#include "osdp/osdp_pd_pair.h"

/* This PD's pairing credential: its certificate, its ML-DSA-44 key pair and
 * the CA it trusts. Generated per device by OSDP-Embedded's
 * osdp-pair-provision and never checked in, because it holds a private key.
 * main/CMakeLists.txt stops the build with instructions if it is missing. */
#include "pair_credentials.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "sc2";

_Static_assert(OSDP_PAIR_SCBK_LEN == SC2_KEY_LEN,
               "pairing derives a key of a different length than the SC2 "
               "key store holds");
_Static_assert(sizeof(osdp_pair_device_pk) == OSDP_MLDSA44_PK_LEN,
               "pair_credentials.h's public key is not an ML-DSA-44 key");
_Static_assert(sizeof(osdp_pair_ca_pk) == OSDP_MLDSA44_PK_LEN,
               "pair_credentials.h's CA key is not an ML-DSA-44 key");

/* Several KB of message buffers, so static rather than on any stack. */
static osdp_pd_pair_t s_pair;

/* Pairing has verified the ACU and derived a key; the PD has not yet told
 * the ACU it succeeded. Saving the key here, before that reply, means a
 * reported success always survives a power cycle. Returning false makes the
 * PD answer PersistenceFailed and throw the key away, so the ACU never
 * believes in a key that the reader won't have after a power cycle.
 *
 * Runs inside osdp_pd_tick(), so it must not call back into the PD API. */
static bool on_paired(void                   *user,
                      const osdp_pair_peer_t *peer,
                      const uint8_t           scbk[OSDP_PAIR_SCBK_LEN])
{
    (void)user;

    /* Who, not what: the peer's identity is public and worth having in the
     * log, and the key is neither. */
    ESP_LOGI(TAG, "paired with %.*s / %.*s / %.*s",
             (int)peer->manufacturer_len, peer->manufacturer,
             (int)peer->model_len, peer->model,
             (int)peer->serial_len, peer->serial);

    if (sc_key_store_sc2(scbk) != ESP_OK) {
        ESP_LOGE(TAG, "could not store the paired key; refusing the pairing");
        return false;
    }
    return true;
}

void sc2_bind(osdp_pd_t *pd, const uint8_t cuid[OSDP_SC2_CUID_LEN])
{
    const osdp_sc2_crypto_t *link = sc2_crypto_link();
    if (link == NULL) {
        ESP_LOGE(TAG, "SC2 is built in but has no crypto backend yet; SC2 "
                      "and pairing stay off");
        return;
    }
    osdp_pd_set_sc2_crypto(pd, link);
    osdp_pd_set_sc2_cuid(pd, cuid);

    uint8_t scbk[SC2_KEY_LEN];
    switch (sc_key_load_sc2(scbk)) {
    case SC_KEY_LOADED:
        osdp_pd_set_sc2_scbk(pd, scbk);
        ESP_LOGI(TAG, "SC2: paired key loaded (%s)",
                 sc_key_protection() == SC_KEY_PROT_EFUSE
                     ? "eFuse-wrapped" : "stored in the clear");
        break;

    case SC_KEY_NONE:
        ESP_LOGW(TAG, "SC2: not paired yet; waiting for an ACU to pair");
        break;

    case SC_KEY_UNREADABLE:
    default:
        memset(scbk, 0, sizeof(scbk));
        ESP_LOGE(TAG, "SC2: a paired key is stored and will not come back. "
                      "Refusing SC2 and pairing rather than pairing over it. "
                      "Hold the key-reset button to clear it.");
        return;
    }
    /* The library copied it. */
    memset(scbk, 0, sizeof(scbk));

    const osdp_pair_crypto_t *pair_crypto =
        sc2_crypto_pair(osdp_pair_device_pk, osdp_pair_device_sk,
                        sizeof(osdp_pair_device_sk));
    if (pair_crypto == NULL) {
        ESP_LOGE(TAG, "pairing crypto unavailable; SC2 runs only on a key "
                      "already stored");
        return;
    }

    const osdp_pair_local_t local = {
        .cert     = osdp_pair_device_cert,
        .cert_len = sizeof(osdp_pair_device_cert),
    };
    const osdp_pair_trust_t trust = { .ca_pubkey = osdp_pair_ca_pk };

    osdp_pd_pair_init(&s_pair, pair_crypto, &local, &trust);
    osdp_pd_pair_set_established_handler(&s_pair, on_paired, NULL);
#if CONFIG_OPENREADER_SC2_DENY_REPAIR
    osdp_pd_pair_set_deny_repair(&s_pair, true);
#endif
    osdp_pd_attach_pair(pd, &s_pair);

    /* osdp-pair-provision only issues credentials under the OSDP.Net demo
     * CA, whose private key anyone can derive. Until this reader has a real
     * CA behind it, any device on the bus can pair with it. */
    ESP_LOGW(TAG, "pairing enabled as %s under the DEMO CA — anyone can "
                  "issue a certificate it trusts",
             OSDP_PAIR_CREDENTIALS_SUBJECT);
}
