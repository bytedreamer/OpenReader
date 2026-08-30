#include "osdp_reader.h"
#include "rs485.h"
#include "status_led.h"
#include "display.h"

#include "osdp/osdp_pd.h"
#include "osdp/osdp_commands.h"
#include "osdp/osdp_replies.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time — diagnostic cadence */
#include "sdkconfig.h"

#include <string.h>
#include <stdio.h>              /* snprintf — first-bytes hex dump */

static const char *TAG = "osdp";

/* This PD has exactly one reader head. OSDP numbers them from 0. */
#define READER_NO 0

/* Format code for the osdp_RAW replies we send.
 *
 * 0x00 (OSDP_RAW_FORMAT_RAW) says "these are unformatted bits, you decide"
 * and is what this reader sends: it reports the MIFARE UID exactly as the
 * RC522 read it and leaves interpretation to the ACU.
 *
 * The spec also defines 0x02 (OSDP_RAW_FORMAT_UID), which describes this
 * payload more precisely. Some controllers understand it, more do not. If
 * yours does, switching this to OSDP_RAW_FORMAT_UID is the more honest
 * wire representation and costs nothing else. */
#define RAW_FORMAT OSDP_RAW_FORMAT_RAW

/* Sized for a comfortable number of queued card reads. Each record costs
 * its payload plus a small length prefix, and an osdp_RAW carrying a 7-byte
 * UID is 11 bytes of payload — so this holds far more than the freshness
 * window could ever accumulate. */
static uint8_t s_event_queue[256];

static osdp_pd_t   s_pd;
static QueueHandle_t s_card_queue;

/* Tamper and power are reported through the status provider. Neither has
 * hardware behind it on a bare dev board: there is no tamper switch and no
 * way to notice a supply brownout before it resets us. They are wired up
 * anyway because an ACU asks, and answering "normal" honestly is better
 * than NAKing osdp_LSTAT — but a production enclosure should drive
 * s_tamper from a real switch. */
static uint8_t s_tamper = OSDP_LSTATR_NORMAL;
static uint8_t s_power  = OSDP_LSTATR_NORMAL;

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

        case PDCAP_FN_AUDIBLE_OUTPUT:
            /* No buzzer on this board. We still accept and decode osdp_BUZ
             * (the library does it for us) but there is nothing to sound,
             * so claiming one would be a lie the ACU might act on. */
            records[i].compliance_level = 0;
            records[i].num_objects      = 0;
            break;

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
    display_set_led(color);
}

static void buzzer_handler(void *user, uint8_t reader_no, bool sounding,
                           uint8_t tone)
{
    (void)user;
    (void)reader_no;
    (void)tone;
    /* No sounder on this board. Logged at debug so a builder adding a
     * piezo can see the pattern edges the library is already resolving for
     * them — the wiring is all that is missing. */
    ESP_LOGD(TAG, "buzzer %s", sounding ? "on" : "off");
}

static void status_local(void *user, uint8_t *tamper, uint8_t *power)
{
    (void)user;
    *tamper = s_tamper;
    *power  = s_power;
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

esp_err_t osdp_reader_submit_card(const rc522_uid_t *uid)
{
    if (uid == NULL || s_card_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(s_card_queue, uid, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void drain_card_queue(void)
{
    rc522_uid_t uid;

    while (xQueueReceive(s_card_queue, &uid, 0) == pdTRUE) {
        /* A card read is only meaningful to an ACU that is actually
         * polling. If we are offline there is nobody to tell, and holding
         * the read until the link returns is exactly the replay the spec
         * warns about (7.11/7.12) — the library would discard it on the
         * offline transition anyway, so drop it here and say why. */
        if (!osdp_pd_is_online(&s_pd)) {
            ESP_LOGW(TAG, "card read dropped: PD offline");
            continue;
        }

        uint8_t body[OSDP_RAW_HEADER_BYTES + RC522_UID_MAX_BYTES];
        size_t  written = 0;

        const osdp_raw_t raw = {
            .reader_no    = READER_NO,
            .format_code  = RAW_FORMAT,
            .bit_count    = (uint16_t)(uid.len * 8U),
            .bit_data     = uid.bytes,
            .bit_data_len = uid.len,
        };

        if (osdp_raw_build(&raw, body, sizeof(body), &written) != OSDP_OK) {
            ESP_LOGE(TAG, "osdp_RAW build failed for a %u-byte UID",
                     (unsigned)uid.len);
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

esp_err_t osdp_reader_init(void)
{
    s_card_queue = xQueueCreate(8, sizeof(rc522_uid_t));
    if (s_card_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

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

    /* Secure Channel is deliberately not configured yet. Without a bound
     * crypto vtable the PD behaves as a clear-text device and none of the
     * SC code is even reachable — see README.md, "Adding Secure Channel",
     * for what to add here. Do not deploy this as-is. */

    ESP_LOGI(TAG, "PD address 0x%02X, %d baud, clear text (no Secure Channel)",
             (unsigned)CONFIG_OPENREADER_OSDP_ADDRESS,
             CONFIG_OPENREADER_RS485_BAUD);
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

void osdp_reader_run(void)
{
    bool     was_online = false;
    uint32_t last_diag_ms = 0;

    for (;;) {
        osdp_pd_tick(&s_pd);
        drain_card_queue();

        bool online = osdp_pd_is_online(&s_pd);
        if (online != was_online) {
            ESP_LOGI(TAG, "link %s", online ? "online" : "offline");
            status_led_set_link(online);
            display_set_link(online);
            was_online = online;
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
