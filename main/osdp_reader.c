#include "osdp_reader.h"
#include "rs485.h"
#include "status_led.h"
#include "display.h"
#include "buzzer.h"
#include "tamper.h"
#include "sc_key.h"
#include "key_reset.h"

#include "osdp/osdp_pd.h"
#include "osdp/osdp_commands.h"
#include "osdp/osdp_replies.h"
#include "osdp/osdp_sc.h"       /* OSDP_SCBK_DEFAULT, OSDP_SC_CUID_LEN */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time — diagnostic cadence */
#include "esp_system.h"         /* esp_reset_reason — restart reporting */
#include "esp_random.h"         /* esp_fill_random — the SC RNG */
#include "bootloader_random.h"  /* entropy for it, on a build without PKOC */
#include "mbedtls/aes.h"        /* AES-128 ECB — the whole of SC's crypto */
#include "sdkconfig.h"

#include <string.h>
#include <stdio.h>              /* snprintf — first-bytes hex dump */

static const char *TAG = "osdp";

/* This PD has exactly one reader head. OSDP numbers them from 0. */
#define READER_NO 0

/* Format code for the osdp_RAW replies we send.
 *
 * 0x00 (OSDP_RAW_FORMAT_RAW) says "these are unformatted bits, you decide",
 * which is what this reader sends whichever kind of credential it read: the
 * MIFARE UID exactly as the RC522 saw it, or the bits AsymCred derived from
 * a verified PKOC public key. Interpretation is the ACU's.
 *
 * The spec also defines 0x02 (OSDP_RAW_FORMAT_UID), which would describe a
 * UID read more precisely — but only a UID read, and a reader that changed
 * format code depending on the card presented would be handing the ACU a
 * moving target. One code for everything is the more useful contract, and
 * it is the one more controllers understand. */
#define RAW_FORMAT OSDP_RAW_FORMAT_RAW

/* Sized for a comfortable number of queued card reads. Each record costs
 * its payload plus a small length prefix. The widest thing this reader can
 * produce is a 256-bit PKOC credential, 36 bytes of payload with the
 * osdp_RAW header; a 7-byte UID is 11. So this holds several of the worst
 * case and many of the common one, which is far more than the card
 * freshness window could ever accumulate. */
static uint8_t s_event_queue[256];

static osdp_pd_t   s_pd;
static QueueHandle_t s_card_queue;

/* The tamper byte the ACU sees, in the spec's own encoding.
 *
 * Backed by a real switch when one is built — tamper.c drives this from the
 * OSDP loop below. Without CONFIG_OPENREADER_TAMPER there is no switch to
 * read and it stays at "normal", which is what a bare dev board can honestly
 * say and better than NAKing osdp_LSTAT. */
static uint8_t s_tamper = OSDP_LSTATR_NORMAL;

/* Power used to report a hard-coded "normal" forever, so a PD that had just
 * restarted looked to the ACU exactly like one that had been running all
 * week. Every restart is now reported, whatever caused it.
 *
 * Reporting a software restart through a byte the spec calls "power failure"
 * is a deliberate stretch. The justification is that it is the only channel
 * OSDP gives a PD to say "I am not the same running instance you were
 * talking to a moment ago", and that is what the head end actually needs to
 * know: credentials in flight were lost, any cached state is gone, and a
 * reader that restarts repeatedly is a fault whatever is causing it. An ACU
 * that can see the restart can act; one that cannot, cannot. A narrower
 * reading — only ESP_RST_POWERON / BROWNOUT / PWR_GLITCH — would leave a
 * crash-looping PD looking perfectly healthy, which is the worse failure.
 *
 * The cause is still logged locally at boot, so "was it the supply?" stays
 * answerable at the bench even though the wire cannot express it.
 *
 * Latched, not live: set at boot and held until the ACU has actually been
 * told, because an ACU that polls osdp_LSTAT rarely would otherwise never
 * learn of the restart at all. */
static bool s_restart_event;    /* restart not yet reported to the ACU      */
static bool s_restart_queued;   /* an unsolicited osdp_LSTATR is in the queue */

/* Which key, if any, this PD is prepared to handshake on.
 *
 * osdp_pd_sc_established() answers "is a session up", and that one bit
 * cannot distinguish the four situations that all present as a light that is
 * not green: no crypto bound, a damaged key store, a reader nobody has keyed
 * yet, and a reader whose key the ACU disagrees with. They want completely
 * different things done about them, so the reason is tracked here and the
 * liveness is read from the library.
 *
 * SC_MODE_INSTALL deserves its own name rather than being folded in with
 * SC_MODE_KEYED, because a session established on SCBK-D is worth nothing:
 * the key is a constant printed in the specification. Calling both of them
 * "secure" would report the mechanism and hide the security. */
typedef enum {
    SC_MODE_OFF = 0,    /* no crypto bound; this PD speaks clear text     */
    SC_MODE_INSTALL,    /* keyed with SCBK-D, waiting for an osdp_KEYSET  */
    SC_MODE_KEYED,      /* an operational SCBK is bound; SCBK-D refused   */
    SC_MODE_FAULT,      /* crypto bound, no usable key — see bind_sc()    */
} sc_mode_t;

static sc_mode_t s_sc_mode = SC_MODE_OFF;

#if CONFIG_OPENREADER_SECURE_CHANNEL
/* Set when an osdp_KEYSET was refused but the library rotated its in-RAM
 * SCBK regardless, leaving the key the PD would handshake on and the key in
 * flash disagreeing. Cleared by reconcile_scbk() on the next tick.
 *
 * The divergence is a property of the library as it stands, not of anything
 * this file does. osdp_pd_internal_dispatch applies the rotation whenever
 * the dispatch outcome is SEND, and a NAK is a perfectly good thing to send
 * — so a handler that refuses a KEYSET still gets its key rotated
 * underneath it, and the wire says one thing while the PD believes another.
 * PD_GUIDE.md documents the opposite ("the PD NAKs and nothing rotates"),
 * which is what the behaviour should be; until it is, this cleans up.
 *
 * It only fires on a failed flash write, which is rare enough that a
 * reconciliation one tick later is a better trade than holding a second copy
 * of the key in this module purely to be able to put it back synchronously. */
static bool s_keyset_diverged;
#endif

/* ---- Identity ----------------------------------------------------------- */

static void build_pdid(osdp_pdid_t *id)
{
    memset(id, 0, sizeof(*id));

    /* Vendor code is an IEEE OUI. 0x00-0x00-0x00 is not assigned to anyone,
     * which is the correct thing for a self-built reader to say: it claims
     * no manufacturer identity. Replace it with your own OUI if you have
     * one — some ACUs key device profiles off this field. */
    id->vendor_code[0] = 0x00;
    id->vendor_code[1] = 0x00;
    id->vendor_code[2] = 0x00;

    id->model   = 0x01;   /* OpenReader ESP32-C6 */
    id->version = 0x01;
    id->serial  = (uint32_t)CONFIG_OPENREADER_PDID_SERIAL;

    id->firmware_major = 0;
    id->firmware_minor = 1;
    id->firmware_build = 0;
}

/* ---- Capabilities ------------------------------------------------------- */

/* Annex B function codes we adjust below. Codes 8, 9 and 10 are reserved —
 * the library computes those itself from its own build configuration and
 * live state, and osdp_pd_set_pdcap rejects any attempt to supply them. */
#define PDCAP_FN_READER_LED        4
#define PDCAP_FN_AUDIBLE_OUTPUT    5
#define PDCAP_FN_LARGEST_COMBINED 11

static void bind_pdcap(void)
{
    osdp_pdcap_record_t records[OSDP_PD_MAX_PDCAP_RECORDS];
    size_t count = 0;

    osdp_status_t st = osdp_pd_pdcap_template(
        OSDP_PDCAP_TEMPLATE_SECURE_READER, records,
        sizeof(records) / sizeof(records[0]), &count);
    if (st != OSDP_OK) {
        ESP_LOGE(TAG, "PDCAP template failed (%d); osdp_CAP will NAK", st);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        switch (records[i].function_code) {
        case PDCAP_FN_LARGEST_COMBINED:
            /* The template leaves this at 0xFFFF as a placeholder meaning
             * "however much this PD can reassemble". We bind no multi-part
             * osdp_MFG receiver, so the honest answer is zero — and
             * osdp_pd_set_pdcap would reject the placeholder anyway. */
            records[i].compliance_level = 0;
            records[i].num_objects      = 0;
            break;

#if !CONFIG_OPENREADER_BUZZER
        case PDCAP_FN_AUDIBLE_OUTPUT:
            /* No sounder fitted in this build. osdp_BUZ is still accepted
             * and decoded (the library does it for us) but there is nothing
             * to make a noise, so claiming one would be a lie the ACU might
             * act on. With CONFIG_OPENREADER_BUZZER the template's own value
             * stands: compliance level 2, "timed commands supported", which
             * is honest because the library really does resolve the
             * on/off/count pattern. */
            records[i].compliance_level = 0;
            records[i].num_objects      = 0;
            break;
#endif

        case PDCAP_FN_READER_LED:
            /* One LED on the reader head. The template's compliance level
             * describes what the LED can do; the single WS2812 can show any
             * of the spec's colours, so the template's value is left alone.
             * Only the count is ours to state. */
            records[i].num_objects = 1;
            break;

        default:
            break;
        }
    }

    size_t bad = 0;
    st = osdp_pd_set_pdcap(&s_pd, records, count, &bad);
    if (st != OSDP_OK) {
        ESP_LOGW(TAG, "PDCAP rejected at record %u (fn %u, status %d) — "
                      "retrying with the unmodified template",
                 (unsigned)bad, (unsigned)records[bad].function_code, st);

        /* Fall back to the template with only the fn-11 fix, which is the
         * one change the library requires rather than one we chose. A PD
         * that answers osdp_CAP with slightly generous capabilities is far
         * more useful than one that NAKs it. */
        (void)osdp_pd_pdcap_template(OSDP_PDCAP_TEMPLATE_SECURE_READER,
                                     records,
                                     sizeof(records) / sizeof(records[0]),
                                     &count);
        for (size_t i = 0; i < count; i++) {
            if (records[i].function_code == PDCAP_FN_LARGEST_COMBINED) {
                records[i].compliance_level = 0;
                records[i].num_objects      = 0;
            }
        }
        st = osdp_pd_set_pdcap(&s_pd, records, count, &bad);
        if (st != OSDP_OK) {
            ESP_LOGE(TAG, "PDCAP fallback failed too (%d)", st);
            return;
        }
    }
    ESP_LOGI(TAG, "PDCAP bound: %u records", (unsigned)count);
}

/* ---- Secure Channel ------------------------------------------------------
 *
 * OSDP-SC (Annex D) reduces entirely to AES-128 on single 16-byte blocks
 * plus a source of randomness. Key derivation, both cryptograms, the initial
 * R-MAC, the rolling MAC and the CBC payload encryption are all built out of
 * those two primitives by the library, which vendors neither — so this is
 * the whole of the crypto this reader supplies.
 *
 * mbedTLS ships with ESP-IDF and its AES is backed by the C6's accelerator,
 * and esp_fill_random() is the hardware RNG. Nothing here is a dependency
 * the build did not already have for PKOC.
 */
#if CONFIG_OPENREADER_SECURE_CHANNEL

/* One block, one direction.
 *
 * The key arrives on every call and the context is set up and torn down
 * around each one. That looks wasteful and is not worth avoiding: the vtable
 * is keyless by design — the library uses several different keys (the SCBK
 * during the handshake, then S-ENC and the two MAC keys) and a cached
 * schedule would have to be invalidated correctly on every one of those
 * transitions. An AES-128 key schedule is a few hundred cycles against a
 * poll cadence measured in milliseconds, and a stale schedule would present
 * as an intermittently unverifiable MAC. Set it up every time.
 *
 * mbedtls_aes_free() on the way out matters for more than tidiness: it zeroes
 * the expanded round keys, so an aborted session does not leave a schedule
 * derived from the SCBK sitting in a stack frame. */
static osdp_status_t aes_block(bool encrypt,
                               const uint8_t key[OSDP_AES_KEY_LEN],
                               const uint8_t in [OSDP_AES_BLOCK_LEN],
                               uint8_t       out[OSDP_AES_BLOCK_LEN])
{
    if (key == NULL || in == NULL || out == NULL) {
        return OSDP_ERR_INVALID_ARG;
    }

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    const int keyed = encrypt
        ? mbedtls_aes_setkey_enc(&ctx, key, OSDP_AES_KEY_LEN * 8U)
        : mbedtls_aes_setkey_dec(&ctx, key, OSDP_AES_KEY_LEN * 8U);

    osdp_status_t st = OSDP_ERR_INVALID_ARG;
    if (keyed == 0 &&
        mbedtls_aes_crypt_ecb(&ctx,
                              encrypt ? MBEDTLS_AES_ENCRYPT
                                      : MBEDTLS_AES_DECRYPT,
                              in, out) == 0) {
        st = OSDP_OK;
    }
    mbedtls_aes_free(&ctx);
    return st;
}

static osdp_status_t sc_encrypt(void *user, const uint8_t key[OSDP_AES_KEY_LEN],
                                const uint8_t in [OSDP_AES_BLOCK_LEN],
                                uint8_t       out[OSDP_AES_BLOCK_LEN])
{
    (void)user;
    return aes_block(true, key, in, out);
}

static osdp_status_t sc_decrypt(void *user, const uint8_t key[OSDP_AES_KEY_LEN],
                                const uint8_t in [OSDP_AES_BLOCK_LEN],
                                uint8_t       out[OSDP_AES_BLOCK_LEN])
{
    (void)user;
    return aes_block(false, key, in, out);
}

/* The PD's half of the handshake nonce, RND.B.
 *
 * This is the one place in Secure Channel where the reader's own randomness
 * is load-bearing: a predictable RND.B lets anyone who has recorded one
 * session replay it. esp_fill_random() draws from the hardware RNG, which on
 * this part is a true RNG only once either the RF subsystem or the
 * bootloader's SAR-ADC entropy source is running, and this firmware brings
 * up no radio. pkoc.c turns that source on for its own nonce and does it
 * before the OSDP task starts; on a build with PKOC compiled out, bind_sc()
 * below turns it on instead. Either way it is running well before an ACU can
 * ask for a handshake. */
static osdp_status_t sc_random(void *user, uint8_t *out, size_t len)
{
    (void)user;
    if (out == NULL) {
        return OSDP_ERR_INVALID_ARG;
    }
    esp_fill_random(out, len);
    return OSDP_OK;
}

static const osdp_sc_crypto_t kCrypto = {
    .aes128_ecb_encrypt = sc_encrypt,
    .aes128_ecb_decrypt = sc_decrypt,
    .rand_bytes         = sc_random,
    .user               = NULL,
};

/* The cUID the ACU addresses this PD by during the handshake.
 *
 * Spec D.4.3 defines it as the first eight bytes of the osdp_PDID byte
 * stream: vendor code, model, version, and the low three bytes of the serial
 * number. Built by encoding the real PDID and taking that prefix rather than
 * by assembling eight bytes by hand — the two must agree, and deriving one
 * from the other is what stops them drifting when build_pdid() changes.
 *
 * Note what this means for CONFIG_OPENREADER_PDID_SERIAL: with the vendor
 * code at 0x000000 and the model and version fixed, the serial is the only
 * thing separating two readers' cUIDs. Leaving every unit on a bus at the
 * default 1 gives them all the same one. */
static bool build_cuid(uint8_t cuid[OSDP_SC_CUID_LEN])
{
    osdp_pdid_t id;
    uint8_t     stream[OSDP_PDID_PAYLOAD_BYTES];
    size_t      written = 0;

    build_pdid(&id);
    if (osdp_pdid_build(&id, stream, sizeof(stream), &written) != OSDP_OK ||
        written < OSDP_SC_CUID_LEN) {
        return false;
    }
    memcpy(cuid, stream, OSDP_SC_CUID_LEN);
    return true;
}

/* Withdraw SCBK-D, so a keyed reader stops answering the published key.
 *
 * This reaches into the library's context instead of calling a setter,
 * because there is no setter for it: osdp_pd_set_sc_scbk_d installs a key
 * and nothing removes one. The struct is a public, documented type in
 * osdp_pd.h — not an opaque handle — so the write is defined behaviour, but
 * it is still a gap in the API rather than an intended use of it.
 *
 * It matters at exactly one moment and it matters a great deal. When an
 * osdp_KEYSET lands, the spec leaves the live session running and the ACU
 * re-handshakes when it chooses. Between those two events the PD would still
 * accept key selector 0 — and an attacker watching the bus has just seen the
 * one command that tells them the reader is now worth re-handshaking with a
 * key they already know. */
static void withdraw_scbk_d(void)
{
    s_pd.sc.scbk_d_set = false;
    memset(s_pd.sc.scbk_d, 0, sizeof(s_pd.sc.scbk_d));
}

/* Decide which key this PD comes up on, and bind it.
 *
 * The three-way split is the whole install-mode policy in one place:
 *
 *   nothing stored     answer on SCBK-D so a panel can reach a fresh reader
 *                      and give it a real key. Install mode.
 *   a key came back    answer on that key ALONE. SCBK-D is never bound, so
 *                      being keyed actually means something.
 *   stored, unreadable answer on nothing at all, and say so loudly.
 *
 * That last case is the one worth defending. Falling back to install mode
 * when the key store looks damaged would hand anyone who can corrupt flash a
 * downgrade to a key printed in the specification — the reader would repair
 * itself straight into being insecure. So an unreadable store refuses every
 * handshake, and the only way back is the button in key_reset.h. */
static void bind_sc(void)
{
#if !CONFIG_OPENREADER_PKOC
    /* Turn on an entropy source, because on this build nothing else has.
     *
     * esp_random() is only a true RNG while the RF subsystem is running, and
     * this firmware never brings up Wi-Fi or Bluetooth — so without this it
     * returns a repeatable sequence. A predictable RND.B lets anyone who
     * recorded one handshake replay it, which would leave the Secure Channel
     * looking entirely healthy while providing none of what it is for.
     *
     * pkoc.c enables the same SAR-ADC source for the same reason, and does it
     * before this runs, so a PKOC build must not call it a second time —
     * regi2c_saradc_enable() behind it is reference counted. Nothing in this
     * firmware uses the ADC, so wherever it is turned on it stays on. */
    bootloader_random_enable();
#endif

    uint8_t cuid[OSDP_SC_CUID_LEN];
    if (!build_cuid(cuid)) {
        ESP_LOGE(TAG, "could not derive the cUID from the PDID; Secure "
                      "Channel stays off");
        return;
    }

    osdp_pd_set_sc_crypto(&s_pd, &kCrypto);
    osdp_pd_set_sc_cuid(&s_pd, cuid);

    uint8_t scbk[SC_KEY_LEN];
    const sc_key_state_t stored = sc_key_load(scbk);

    switch (stored) {
    case SC_KEY_LOADED:
        osdp_pd_set_sc_scbk(&s_pd, scbk);
        withdraw_scbk_d();
        s_sc_mode = SC_MODE_KEYED;
        ESP_LOGI(TAG, "Secure Channel: operational SCBK loaded (%s); "
                      "SCBK-D refused",
                 sc_key_protection() == SC_KEY_PROT_EFUSE
                     ? "eFuse-wrapped" : "stored in the clear");
        break;

    case SC_KEY_NONE:
#if CONFIG_OPENREADER_SC_INSTALL_MODE
        osdp_pd_set_sc_scbk_d(&s_pd, OSDP_SCBK_DEFAULT);
        s_sc_mode = SC_MODE_INSTALL;
        ESP_LOGW(TAG, "Secure Channel: INSTALL MODE — answering on SCBK-D, "
                      "the key from spec D.4 that everyone knows. Send "
                      "osdp_KEYSET to key this reader.");
#else
        s_sc_mode = SC_MODE_FAULT;
        ESP_LOGE(TAG, "Secure Channel: no key stored and install mode is "
                      "compiled out — this reader cannot handshake with "
                      "anything");
#endif
        break;

    case SC_KEY_UNREADABLE:
    default:
        s_sc_mode = SC_MODE_FAULT;
        ESP_LOGE(TAG, "Secure Channel: a key is stored and will not come "
                      "back. Refusing every handshake rather than falling "
                      "back to SCBK-D. Hold the key-reset button to return "
                      "this reader to install mode.");
        break;
    }

    /* The library copied what it needs; this stack frame should not still be
     * holding the SCBK when the next call reuses it. */
    memset(scbk, 0, sizeof(scbk));
}

/* Put the PD's in-RAM SCBK back in step with what is actually stored.
 *
 * Called from the tick loop after a refused osdp_KEYSET — see
 * s_keyset_diverged for why one can be refused and rotated at the same time.
 * The store is the source of truth, always: what survives a power cycle is
 * what the reader is, and anything in RAM that disagrees is the thing that
 * is wrong.
 *
 * Restoring after the fact rather than preventing the rotation is safe
 * because the rotation only affects the NEXT handshake. Any live session
 * keeps running on session keys already derived, and the ACU cannot start a
 * new handshake between the NAK going out and this running one tick later —
 * that would take a round trip, and this is a millisecond away. */
static void reconcile_scbk(void)
{
    uint8_t scbk[SC_KEY_LEN];
    const sc_key_state_t stored = sc_key_load(scbk);

    if (stored == SC_KEY_LOADED) {
        osdp_pd_set_sc_scbk(&s_pd, scbk);
        ESP_LOGW(TAG, "restored the stored SCBK after a refused osdp_KEYSET");
    } else {
        /* Nothing usable is stored, so the PD must not be holding a key it
         * would handshake on. Clearing the flag is the same reach past the
         * API as withdraw_scbk_d(), and for the same missing setter. */
        s_pd.sc.scbk_set = false;
        memset(s_pd.sc.scbk, 0, sizeof(s_pd.sc.scbk));
        ESP_LOGW(TAG, "cleared the rotated SCBK after a refused osdp_KEYSET; "
                      "nothing is stored");
    }

    memset(scbk, 0, sizeof(scbk));
    s_keyset_diverged = false;
}
#endif /* CONFIG_OPENREADER_SECURE_CHANNEL */

/* ---- Handlers ----------------------------------------------------------- */

/* Names for the codes that can reach the default branch below, so a NAK
 * says what was refused rather than leaving a bare hex byte to look up.
 * LED and BUZ are listed even though the switch now answers them: if a
 * future edit drops those cases, this is the line that will say so. */
static const char *cmd_name(uint8_t code)
{
    switch (code) {
    case OSDP_CMD_LED:          return "osdp_LED";
    case OSDP_CMD_BUZ:          return "osdp_BUZ";
    case OSDP_CMD_OUT:          return "osdp_OUT";
    case OSDP_CMD_TEXT:         return "osdp_TEXT";
    case OSDP_CMD_BIOREAD:      return "osdp_BIOREAD";
    case OSDP_CMD_BIOMATCH:     return "osdp_BIOMATCH";
    case OSDP_CMD_KEYSET:       return "osdp_KEYSET";
    case OSDP_CMD_CHLNG:        return "osdp_CHLNG";
    case OSDP_CMD_SCRYPT:       return "osdp_SCRYPT";
    case OSDP_CMD_FILETRANSFER: return "osdp_FILETRANSFER";
    case OSDP_CMD_MFG:          return "osdp_MFG";
    case OSDP_CMD_XWR:          return "osdp_XWR";
    case OSDP_CMD_PIVDATA:      return "osdp_PIVDATA";
    case OSDP_CMD_GENAUTH:      return "osdp_GENAUTH";
    case OSDP_CMD_CRAUTH:       return "osdp_CRAUTH";
#ifdef OSDP_CMD_PAIR
    /* Only on library branches that carry the pairing work. Guarded so
     * this file builds against either. */
    case OSDP_CMD_PAIR:         return "osdp_PAIR";
#endif
    default:                    return "unknown";
    }
}

/* One line per distinct code, not one per occurrence. An ACU that wants
 * something we do not implement usually asks for it on every cycle, and a
 * NAK logged at poll rate would bury everything else in the console. */
static void log_first_nak(uint8_t code, const uint8_t *payload, size_t len)
{
    static uint32_t seen[8];

    uint32_t bit = 1UL << (code & 31U);
    if (seen[code >> 5] & bit) {
        return;
    }
    seen[code >> 5] |= bit;

    char hex[3 * 8 + 1];
    size_t shown = len < 8 ? len : 8;
    for (size_t i = 0; i < shown; i++) {
        snprintf(&hex[i * 3], 4, "%02X ", payload[i]);
    }
    hex[shown * 3] = '\0';

    ESP_LOGW(TAG, "NAK 0x03 to 0x%02X (%s), %u payload bytes%s%s",
             code, cmd_name(code), (unsigned)len,
             shown > 0 ? ": " : "", hex);
}

/* Reached for every command the library does not answer entirely on its
 * own. CAP, COMSET, FILETRANSFER, ABORT, ACURXSIZE, KEEPACTIVE and the
 * LSTAT/ISTAT/OSTAT/RSTAT status requests are absorbed upstream and never
 * arrive here.
 *
 * osdp_LED and osdp_BUZ are the trap. The library decodes them into its own
 * LED and buzzer banks and calls led_handler/buzzer_handler for us — but it
 * does that *after* this function returns, and it leaves the reply to the
 * application (pd_dispatch.c pre-fills ACK before the call, then applies
 * whatever status we hand back). So a handler that quietly ignores them
 * still drives the LED correctly while NAKing the very command that drove
 * it: the light does the right thing and the ACU is told the command was
 * not understood. They must be answered here explicitly. */
static osdp_status_t command_handler(void *user, uint8_t code,
                                     const uint8_t *payload, size_t len,
                                     osdp_pd_reply_t *reply)
{
    (void)user;

    /* Scratch for replies that carry a body. Static rather than a local
     * because the PD reads it after we return — it copies into its own TX
     * buffer, but only once the handler has handed the pointer back. */
    static uint8_t body[OSDP_PDID_PAYLOAD_BYTES];

    switch (code) {
    case OSDP_CMD_POLL:
        /* Only reached when the event queue is empty; a queued card read is
         * answered by the library in place of this ACK. */
        reply->code        = OSDP_REPLY_ACK;
        reply->payload     = NULL;
        reply->payload_len = 0;
        return OSDP_OK;

    case OSDP_CMD_LED:
    case OSDP_CMD_BUZ:
        /* Nothing to do but agree. The decode and the callbacks are the
         * library's, and they run once we return; returning OSDP_OK is what
         * turns its pre-filled ACK into the reply that actually goes out.
         *
         * BUZ is ACKed even though no sounder is fitted. The PD really does
         * process the command — it decodes it and resolves the pattern —
         * and osdp_CAP already tells the ACU there are zero audible outputs,
         * which is the honest place to say so. NAK 0x03 would claim we did
         * not understand it, which is a different and untrue statement. */
        reply->code        = OSDP_REPLY_ACK;
        reply->payload     = NULL;
        reply->payload_len = 0;
        return OSDP_OK;

#if CONFIG_OPENREADER_SECURE_CHANNEL
    case OSDP_CMD_KEYSET:
        /* The ACU is giving this reader its operational key, and this is the
         * command that ends install mode.
         *
         * The library rotates its own in-RAM copy once we return OSDP_OK,
         * and persisting the key is ours — get that wrong and the reader
         * works perfectly until the next power cycle and then never speaks
         * to the panel again, which is the worst shape a bug can have here.
         * So the write to flash happens first and its result decides the
         * reply. Nothing is ACKed that is not already stored.
         *
         * Same validation the library will apply, in the same order, so this
         * never persists a key the library is then going to reject. */
        {
            osdp_keyset_cmd_t ks;
            if (osdp_keyset_decode(payload, len, &ks) != OSDP_OK ||
                ks.key_type   != OSDP_KEYSET_KEY_TYPE_SCBK ||
                ks.key_length != OSDP_SC_KEY_LEN ||
                ks.key_data   == NULL) {
                /* NAK 0x09, "unable to process command record" — the right
                 * answer for a command this PD does implement carrying a
                 * record it cannot use. Not 0x03, which would say KEYSET
                 * itself is unknown. */
                ESP_LOGW(TAG, "osdp_KEYSET rejected: not a 16-byte SCBK");
                return OSDP_ERR_INVALID_ARG;
            }

            if (sc_key_store(ks.key_data) != ESP_OK) {
                /* The flash write failed. Refuse the rotation rather than
                 * ACK a key that will not survive the next reboot — and see
                 * s_keyset_diverged, which cleans up after the library
                 * having rotated its RAM copy anyway. */
                s_keyset_diverged = true;
                return OSDP_ERR_INVALID_ARG;
            }

            /* Stored. From here on this reader is keyed: it answers on the
             * new SCBK and refuses the published one. The live session is
             * deliberately left running — spec semantics are that the new
             * key takes effect at the next handshake, which the ACU starts
             * when it chooses. */
            withdraw_scbk_d();
            s_sc_mode = SC_MODE_KEYED;
            ESP_LOGI(TAG, "osdp_KEYSET applied and stored; this reader is "
                          "now keyed and no longer answers SCBK-D");

            reply->code        = OSDP_REPLY_ACK;
            reply->payload     = NULL;
            reply->payload_len = 0;
            return OSDP_OK;
        }
#endif /* CONFIG_OPENREADER_SECURE_CHANNEL */

    case OSDP_CMD_ID: {
        osdp_pdid_t id;
        build_pdid(&id);
        size_t written = 0;
        if (osdp_pdid_build(&id, body, sizeof(body), &written) != OSDP_OK) {
            return OSDP_ERR_INVALID_ARG;   /* -> NAK 0x09 */
        }
        reply->code        = OSDP_REPLY_PDID;
        reply->payload     = body;
        reply->payload_len = written;
        return OSDP_OK;
    }

    default:
        /* NAK 0x03. Answering "I don't implement that" is strictly better
         * than staying silent: the ACU learns immediately instead of
         * waiting out its reply timeout. */
        log_first_nak(code, payload, len);
        return OSDP_ERR_NOT_SUPPORTED;
    }
}

static void led_handler(void *user, uint8_t reader_no, uint8_t led_no,
                        uint8_t color)
{
    (void)user;
    if (reader_no != READER_NO || led_no != 0) {
        return;
    }
    status_led_set_osdp(color);
}

static void buzzer_handler(void *user, uint8_t reader_no, bool sounding,
                           uint8_t tone)
{
    (void)user;
    (void)reader_no;
    /* tone is deliberately ignored. osdp_BUZ defines two codes — 0x01 off
     * and 0x02 default tone — and an active sounder has exactly one noise
     * in it, so there is nothing here to choose between. `sounding` already
     * folds in the off code. */
    (void)tone;

#if CONFIG_OPENREADER_BUZZER
    buzzer_set_osdp(sounding);
#else
    /* Logged at debug so a builder can watch the pattern edges the library
     * is already resolving before committing to the part. */
    ESP_LOGD(TAG, "buzzer %s", sounding ? "on" : "off");
#endif
}

static void status_local(void *user, uint8_t *tamper, uint8_t *power)
{
    (void)user;
    *tamper = s_tamper;
    *power  = s_restart_event ? OSDP_LSTATR_POWER_FAILURE
                              : OSDP_LSTATR_NORMAL;

    /* An explicit osdp_LSTAT counts as the report, so the latch clears here
     * too — not only on the unsolicited path. If the reply is then lost on
     * the wire the ACU repeats the command, and the library replays the
     * cached reply byte-for-byte rather than calling this again, so the
     * retry still carries the power flag. */
    s_restart_event = false;
}

static size_t status_readers(void *user, uint8_t *out, size_t cap)
{
    (void)user;
    if (cap < 1) {
        return 0;
    }
    /* One reader head, and it is present as long as we are running: the
     * RC522 sits on our own PCB, not on a downstream port that could be
     * cut. If rc522_init() failed we would not have got this far. */
    out[0] = OSDP_RSTATR_NORMAL;
    return 1;
}

/* No contact inputs and no relay outputs on this build. Reporting zero of
 * each is legal and truthful, and is better than leaving these NULL: a NULL
 * member sends osdp_ISTAT/osdp_OSTAT to the command handler, which would
 * NAK them. */
static size_t status_inputs(void *user, uint8_t *out, size_t cap)
{
    (void)user; (void)out; (void)cap;
    return 0;
}

static size_t status_outputs(void *user, uint8_t *out, size_t cap)
{
    (void)user; (void)out; (void)cap;
    return 0;
}

/* ---- Card reads --------------------------------------------------------- */

esp_err_t osdp_reader_submit_card(const credential_t *cred)
{
    if (cred == NULL || s_card_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(s_card_queue, cred, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void drain_card_queue(void)
{
    credential_t cred;

    while (xQueueReceive(s_card_queue, &cred, 0) == pdTRUE) {
        /* A card read is only meaningful to an ACU that is actually
         * polling. If we are offline there is nobody to tell, and holding
         * the read until the link returns is exactly the replay the spec
         * warns about (7.11/7.12) — the library would discard it on the
         * offline transition anyway, so drop it here and say why. */
        if (!osdp_pd_is_online(&s_pd)) {
            ESP_LOGW(TAG, "card read dropped: PD offline");
            continue;
        }

        uint8_t body[OSDP_RAW_HEADER_BYTES + CREDENTIAL_MAX_BYTES];
        size_t  written = 0;

        const osdp_raw_t raw = {
            .reader_no    = READER_NO,
            .format_code  = RAW_FORMAT,
            /* The credential carries its own bit count rather than eight
             * times its length: the 75-bit PKOC form occupies ten bytes
             * whose top five bits are zero, and telling the ACU 80 would be
             * telling it about five bits that are not part of anything. */
            .bit_count    = cred.bit_count,
            .bit_data     = cred.bytes,
            .bit_data_len = cred.len,
        };

        if (osdp_raw_build(&raw, body, sizeof(body), &written) != OSDP_OK) {
            ESP_LOGE(TAG, "osdp_RAW build failed for a %u-bit %s credential",
                     (unsigned)cred.bit_count, credential_kind_name(&cred));
            continue;
        }

        osdp_status_t st = osdp_pd_enqueue_event(&s_pd, OSDP_REPLY_RAW,
                                                 body, written);
        if (st != OSDP_OK) {
            /* Only this application knows whether a dropped read matters.
             * For a door reader it does: the holder will present the card
             * again, but they deserve to know why nothing happened, so this
             * is a warning rather than a debug line. */
            ESP_LOGW(TAG, "event queue full; card read dropped");
        }
    }
}

/* ---- Lifecycle ---------------------------------------------------------- */

/* Record that we restarted. Called once, before the PD is serviced.
 *
 * Reaching this function at all means a restart, so the latch is set
 * unconditionally; esp_reset_reason() is read for the log rather than to
 * decide anything. Keeping the cause visible locally is what lets someone
 * at the bench tell a power cycle from a panic, a distinction the ACU is
 * given no way to see. */
static void note_reset_reason(void)
{
    static const char *const kReason[] = {
        "unknown", "power-on", "external pin", "software restart", "panic",
        "interrupt watchdog", "task watchdog", "other watchdog",
        "deep sleep", "brownout", "SDIO", "USB", "JTAG", "efuse error",
        "power glitch", "CPU lockup",
    };
    const esp_reset_reason_t why = esp_reset_reason();
    const char *name =
        ((size_t)why < sizeof(kReason) / sizeof(kReason[0]))
            ? kReason[why] : "unrecognised";

    s_restart_event = true;

    ESP_LOGI(TAG, "restart: %s (reason %d) — will report to the ACU",
             name, (int)why);
}

esp_err_t osdp_reader_init(void)
{
    s_card_queue = xQueueCreate(8, sizeof(credential_t));
    if (s_card_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    note_reset_reason();

    osdp_pd_init(&s_pd, (uint8_t)CONFIG_OPENREADER_OSDP_ADDRESS);
    osdp_pd_set_transport(&s_pd, rs485_transport());
    osdp_pd_set_command_handler(&s_pd, command_handler, NULL);
    osdp_pd_set_event_queue(&s_pd, s_event_queue, sizeof(s_event_queue));
    osdp_pd_set_led_handler(&s_pd, led_handler, NULL);
    osdp_pd_set_buzzer_handler(&s_pd, buzzer_handler, NULL);

    static const osdp_pd_status_provider_t status = {
        .local   = status_local,
        .inputs  = status_inputs,
        .outputs = status_outputs,
        .readers = status_readers,
    };
    osdp_pd_set_status_provider(&s_pd, &status, NULL);

    bind_pdcap();

#if CONFIG_OPENREADER_SECURE_CHANNEL
    /* After bind_pdcap(), and it has to be. PDCAP function code 9
     * ("Communication Security") is one of the three records the library
     * computes for itself, and its object count reports which key this PD is
     * carrying — 0x00 once an operational SCBK is set, 0x01 while it is
     * still on SCBK-D. It is recomputed on every osdp_CAP rather than cached
     * at bind time, so the ordering here is not load-bearing for
     * correctness; it is written this way because the reader is not really
     * configured until its keys are. */
    bind_sc();
#else
    /* Without a bound crypto vtable the PD refuses every SCB-bearing frame
     * with NAK 0x05 and none of the SC code is reachable. That is a working
     * clear-text device and it is not something to put on a door: anyone
     * with access to the cable can watch a credential go past and replay it.
     */
    ESP_LOGE(TAG, "Secure Channel is compiled out — every credential crosses "
                  "this bus in the clear");
#endif

    ESP_LOGI(TAG, "PD address 0x%02X, %d baud, %s",
             (unsigned)CONFIG_OPENREADER_OSDP_ADDRESS,
             CONFIG_OPENREADER_RS485_BAUD,
             s_sc_mode == SC_MODE_KEYED   ? "Secure Channel, keyed"
             : s_sc_mode == SC_MODE_INSTALL ? "Secure Channel, INSTALL MODE"
             : s_sc_mode == SC_MODE_FAULT   ? "Secure Channel, NO USABLE KEY"
                                            : "clear text");
    return ESP_OK;
}

/* Report the wire counters while the link is down.
 *
 * A PD that is not being polled logs nothing at all, which makes "silent"
 * indistinguishable from "silent for four different reasons". These three
 * numbers separate them:
 *
 *   rx=0                  nothing is reaching the pin. Wrong port, A/B
 *                         swapped, the level shifter, or the adapter is not
 *                         actually RS-485.
 *   rx>0, tx=0            bytes are arriving but the PD never answered, so
 *                         no frame was both well-formed and addressed here:
 *                         baud mismatch, or the wrong PD address. Check the
 *                         first bytes — at the right baud an OSDP frame
 *                         starts 0x53.
 *   rx>0, tx>0            the PD is hearing the ACU and replying, so the
 *                         fault is on the return path — the transceiver not
 *                         turning around, or the ACU not seeing us.
 */
static void log_link_diagnostics(void)
{
    uint32_t rx = 0, tx = 0;
    uint8_t  first[16];
    size_t   first_len = 0;
    rs485_stats(&rx, &tx, first, sizeof(first), &first_len);

    if (first_len == 0) {
        ESP_LOGW(TAG, "offline: rx=%lu tx=%lu — nothing received yet; "
                      "emitting bus marker A5 5A A5 5A",
                 (unsigned long)rx, (unsigned long)tx);
        /* Nothing has ever arrived, so the receive path tells us nothing
         * about the transceiver. Talk instead: a monitor elsewhere on the
         * bus seeing this marker proves the chip is powered, its
         * auto-direction turnaround works, and A/B really are connected —
         * none of which the silent receive path can establish. */
        rs485_emit_marker();
        return;
    }

    char hex[sizeof(first) * 3 + 1];
    size_t pos = 0;
    for (size_t i = 0; i < first_len; i++) {
        pos += (size_t)snprintf(&hex[pos], sizeof(hex) - pos, "%02X%s",
                                first[i], (i + 1 < first_len) ? " " : "");
    }

    ESP_LOGW(TAG, "offline: rx=%lu tx=%lu  first bytes: %s%s",
             (unsigned long)rx, (unsigned long)tx, hex,
             (first[0] == 0x53) ? "  (0x53 SOM — baud looks right)"
                                : "  (expected 0x53 first — check baud)");
}

/* Tell the ACU we restarted, without waiting to be asked.
 *
 * osdp_LSTATR is a legal answer to an osdp_POLL, so this rides the same
 * event queue the card reads use and goes out on the very next poll rather
 * than whenever the ACU next happens to send osdp_LSTAT.
 *
 * Deliberately called on the offline->online edge and not at init: the
 * library discards the event queue when the PD drops offline (spec 7.11/
 * 7.12, so a stale credential is never delivered late), and anything queued
 * before the first poll would be thrown away by that same rule. */
static void announce_restart(void)
{
    if (!s_restart_event || s_restart_queued) {
        return;
    }

    const osdp_lstatr_t st = {
        .tamper = s_tamper,
        .power  = OSDP_LSTATR_POWER_FAILURE,
    };
    uint8_t body[OSDP_LSTATR_PAYLOAD_BYTES];
    size_t  written = 0;

    if (osdp_lstatr_build(&st, body, sizeof(body), &written) != OSDP_OK) {
        ESP_LOGE(TAG, "could not build the restart osdp_LSTATR");
        return;
    }
    if (osdp_pd_enqueue_event(&s_pd, OSDP_REPLY_LSTATR, body,
                              written) != OSDP_OK) {
        /* Queue full at the moment the link came up. The latch stays set,
         * so the next reconnect tries again. */
        ESP_LOGW(TAG, "event queue full; restart report deferred");
        return;
    }
    s_restart_queued = true;
    ESP_LOGI(TAG, "queued unsolicited osdp_LSTATR reporting the restart");
}

#if CONFIG_OPENREADER_TAMPER
/* Report a tamper transition without waiting to be asked.
 *
 * Same reasoning as announce_restart(): osdp_LSTATR is a legal answer to an
 * osdp_POLL, so the change reaches the ACU on the very next poll instead of
 * whenever it next happens to send an osdp_LSTAT. A tamper the head end
 * learns about several minutes late is not much of a tamper.
 *
 * Unlike the restart latch this is not retried if the queue is full or the
 * link is down, and it does not need to be. The restart latch exists because
 * a restart is an *event* that leaves no trace once missed; tamper is a
 * *state*. s_tamper still holds it, status_local() still reports it, and the
 * ACU's next osdp_LSTAT gets the truth. Missing the unsolicited report costs
 * latency, not correctness. */
static void announce_tamper(void)
{
    const osdp_lstatr_t st = {
        .tamper = s_tamper,
        .power  = s_restart_event ? OSDP_LSTATR_POWER_FAILURE
                                  : OSDP_LSTATR_NORMAL,
    };
    uint8_t body[OSDP_LSTATR_PAYLOAD_BYTES];
    size_t  written = 0;

    if (osdp_lstatr_build(&st, body, sizeof(body), &written) != OSDP_OK) {
        ESP_LOGE(TAG, "could not build the tamper osdp_LSTATR");
        return;
    }
    if (osdp_pd_enqueue_event(&s_pd, OSDP_REPLY_LSTATR, body,
                              written) != OSDP_OK) {
        ESP_LOGW(TAG, "event queue full; tamper change not reported until "
                      "the next osdp_LSTAT");
        return;
    }
    ESP_LOGI(TAG, "queued unsolicited osdp_LSTATR reporting %s",
             s_tamper == OSDP_LSTATR_TAMPER ? "tamper" : "tamper cleared");
}
#endif /* CONFIG_OPENREADER_TAMPER */

/* What the reader face should say about Secure Channel.
 *
 * Two independent facts crossed: which key this PD is prepared to handshake
 * on, and whether a session is actually up. osdp_pd_sc_established() answers
 * only the second, and on its own it cannot tell a reader nobody has keyed
 * from one whose key the ACU disagrees with — nor, more importantly, a real
 * session from one wrapped under a key printed in the specification. */
static display_sc_t secure_state(void)
{
    const bool up = osdp_pd_sc_established(&s_pd);

    switch (s_sc_mode) {
    case SC_MODE_KEYED:
        return up ? DISPLAY_SC_ACTIVE : DISPLAY_SC_NONE;
    case SC_MODE_INSTALL:
        return up ? DISPLAY_SC_INSTALL_UP : DISPLAY_SC_INSTALL;
    case SC_MODE_FAULT:
        return DISPLAY_SC_FAULT;
    case SC_MODE_OFF:
    default:
        return DISPLAY_SC_CLEAR;
    }
}

#if CONFIG_OPENREADER_KEY_RESET
/* Erase the key and restart, having watched somebody hold a button for ten
 * seconds to ask for it.
 *
 * The restart is the point, not an afterthought. A live Secure Channel
 * session is running on keys derived from an SCBK that no longer exists, and
 * every route to unwinding that in place — clearing the session, re-binding
 * SCBK-D, deciding what to do about the sequence number the ACU is still
 * counting from — is a state transition this PD would then have to be
 * correct about while a panel talks to it. Rebooting reaches the same place
 * along a path that is exercised on every power-up. The ACU sees the reader
 * drop and come back, which is exactly what happened.
 *
 * Fail loudly rather than silently: someone who held the button for ten
 * seconds and got a beep is entitled to believe the key is gone, so if the
 * erase did not take, nothing must look like it did. */
static void do_key_reset(void)
{
    ESP_LOGW(TAG, "key reset requested from the button");

    const esp_err_t err = sc_key_erase();

    status_led_override(err == ESP_OK ? OSDP_LED_GREEN : OSDP_LED_RED);
#if CONFIG_OPENREADER_BUZZER
    buzzer_local(true);
#endif
    /* Long enough to be seen and heard from where the button is, and the
     * only place in this loop that deliberately blocks. Nothing is being
     * serviced during it — which is correct, because the PD is one second
     * away from restarting anyway. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key erase FAILED (%s) — this reader is still keyed",
                 esp_err_to_name(err));
#if CONFIG_OPENREADER_BUZZER
        buzzer_local(false);
#endif
        status_led_override(STATUS_LED_NO_OVERRIDE);
        display_set_key_reset(DISPLAY_NO_KEY_RESET);
        return;
    }

    ESP_LOGW(TAG, "key erased; restarting into install mode");
    esp_restart();
}

/* Show the hold as it runs, and hand the reader back when it is abandoned.
 *
 * Called every tick; almost every call does nothing. The countdown is
 * quantised to whole seconds because the panel repaints whenever its
 * snapshot changes and a millisecond-resolution value would mean a full
 * frame over SPI on every one of these. */
static void show_key_reset_progress(void)
{
    static int last_shown = DISPLAY_NO_KEY_RESET;

    const uint32_t held  = key_reset_held_ms();
    const uint32_t total = key_reset_hold_ms();

    int now = DISPLAY_NO_KEY_RESET;
    if (held > 0) {
        /* Rounded up, so the last whole second shows "1" rather than "0"
         * for the second before it fires — a zero that sits there is a
         * countdown that looks stuck. */
        const uint32_t left = (held >= total) ? 0U : (total - held);
        now = (int)((left + 999U) / 1000U);
    }

    if (now == last_shown) {
        return;
    }
    last_shown = now;

    display_set_key_reset(now);
    status_led_override(now == DISPLAY_NO_KEY_RESET ? STATUS_LED_NO_OVERRIDE
                                                    : OSDP_LED_AMBER);
    if (now != DISPLAY_NO_KEY_RESET) {
        ESP_LOGW(TAG, "key reset in %d s — release to cancel", now);
    } else {
        ESP_LOGI(TAG, "key reset cancelled");
    }
}
#endif /* CONFIG_OPENREADER_KEY_RESET */

void osdp_reader_run(void)
{
    bool         was_online = false;
    display_sc_t was_sc     = DISPLAY_SC_CLEAR;
    uint32_t     last_diag_ms = 0;

    for (;;) {
        osdp_pd_tick(&s_pd);
        drain_card_queue();

#if CONFIG_OPENREADER_SECURE_CHANNEL
        /* Immediately after the tick that dispatched the refused osdp_KEYSET,
         * and before anything else can observe the PD holding a key that is
         * not the stored one. */
        if (s_keyset_diverged) {
            reconcile_scbk();
        }
#endif

#if CONFIG_OPENREADER_KEY_RESET
        show_key_reset_progress();
        if (key_reset_poll()) {
            do_key_reset();
            /* Only reached when the erase failed; do_key_reset() restarts on
             * success. Carry on serving the bus with the key intact. */
        }
#endif

        bool online = osdp_pd_is_online(&s_pd);
        if (online != was_online) {
            ESP_LOGI(TAG, "link %s", online ? "online" : "offline");
            status_led_set_link(online);
            display_set_link(online);
#if CONFIG_OPENREADER_BUZZER
            buzzer_set_link(online);
#endif
            if (online) {
                announce_restart();
            } else {
                /* Going offline empties the event queue, our report with it.
                 * Forget that it was ever queued so the next reconnect
                 * re-sends it — the latch is still set, and an unreported
                 * restart is exactly the thing that should survive a flaky
                 * link rather than be quietly dropped. */
                s_restart_queued = false;
            }
            was_online = online;
        }

        /* Secure Channel establishes (and is torn down by a session loss)
         * without producing any edge the block above would see, so it is
         * tracked on its own. The check is a bool read and a compare; only
         * an actual change reaches the panel. */
        display_sc_t sc = secure_state();
        if (sc != was_sc) {
            display_set_secure(sc);
            was_sc = sc;
        }

#if CONFIG_OPENREADER_TAMPER
        /* Every tick, but the filter in tamper_poll() means this returns
         * true only on an actual debounced transition. The state is recorded
         * whether or not the link is up — status_local() answers with it
         * either way; only the unsolicited report needs somewhere to go. */
        if (tamper_poll()) {
            s_tamper = tamper_active() ? OSDP_LSTATR_TAMPER
                                       : OSDP_LSTATR_NORMAL;
            if (online) {
                announce_tamper();
            }
        }
#endif

        /* The queue draining is the only evidence we get that the report
         * actually went out; there is no per-event transmit hook. Ours is
         * enqueued on the online edge, ahead of any card read, so an empty
         * queue means it has definitely been sent. */
        if (s_restart_queued && online && !osdp_pd_event_pending(&s_pd)) {
            s_restart_event  = false;
            s_restart_queued = false;
            ESP_LOGI(TAG, "restart reported to the ACU; latch cleared");
        }
        status_led_tick();

        /* Only while down, and only every 5 s: a working reader should not
         * be narrating itself, and an ACU that stops polling should leave a
         * trail rather than silence. */
        if (!online) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (last_diag_ms == 0 || (now - last_diag_ms) >= 5000U) {
                log_link_diagnostics();
                last_diag_ms = now;
            }
        }

        /* 1 ms with CONFIG_FREERTOS_HZ=1000. The tick has to be frequent
         * enough that a reply lands well inside the ACU's timeout and that
         * the spec-5.8 inter-character timeout is measured with useful
         * resolution — but it must still yield, or the idle task never runs
         * and the task watchdog fires. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
