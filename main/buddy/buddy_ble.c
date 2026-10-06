#include "buddy_ble.h"
#include "buddy_profile.h"
#include "buddy_proximity.h"
#include "buddy_contacts.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_bt.h"

static const char *TAG = "buddy_ble";

/* ── Profile exchange framing ──────────────────────────────────── */
/* Neither direction of a profile fits one ATT operation (MTU - 3 = 244 bytes
 * here, against several hundred bytes of JSON), and neither of ATT's "long"
 * procedures worked between two copies of this firmware:
 *
 *   Write Long   -> the peer's server answered BLE_ATT_ERR_INVALID_OFFSET
 *   Read Long    -> the peer's server answered BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN
 *
 * So both directions are carried by a windowing scheme built on ordinary writes
 * and reads, which the ATT layer transports without interpretation:
 *
 *   write   ['S'|'E'][payload]   one chunk of my profile ('E' = final chunk)
 *   write   [0x3F][off_lo][off_hi]  "send me the window at this offset"
 *   read    ['S'|'E'][payload]   one window of your profile
 *
 * The marker byte also keeps a window from ever filling the response: the
 * payload stays one byte short of the ceiling. */
#define CHAT_CHUNK_MORE    'S'   /* more chunks follow */
#define CHAT_CHUNK_LAST    'E'   /* final chunk */
#define PROFILE_CMD_GET    0x3F  /* read request (distinct from both markers) */

/* Window size the reader asks for.  The reply is this plus the marker, so it
 * stays inside BUDDY_PROF_CHUNK_MAX and therefore inside MTU - 3. */
#define PROFILE_READ_WINDOW  (BUDDY_PROF_CHUNK_MAX - 1)

/* ── Peer tracking (beacon dedup + handshake cooldown) ────────── */
#define PEER_TRACK_MAX 32

typedef struct {
    uint8_t  mac[6];
    char     device_id[18];
    int8_t   rssi;
    int64_t  last_ad_ms;
    int64_t  last_conn_ms;
    bool     blocked;
} peer_track_t;

static peer_track_t *s_peers = NULL;
static int s_peer_count = 0;

/* ── Connection state (single active connection) ───────────────── */
typedef struct {
    uint16_t conn_handle;
    uint8_t  peer_mac[6];
    char     peer_device_id[18];
    int8_t   rssi;
    char     profile_buf[BUDDY_PROFILE_MAX_BYTES];
    size_t   profile_len;
    bool     profile_sent;
    bool     active;
    bool     outgoing;   /* true = we initiated this connection */
    uint8_t  peer_flags; /* advertising flags the peer broadcast */
} buddy_ble_conn_t;

/* ── Module state ──────────────────────────────────────────────── */
static QueueHandle_t s_event_queue = NULL;
static QueueHandle_t s_chat_queue = NULL;
static uint8_t s_own_addr_type = 0;
/* advertising runs forever while s_running */
static bool s_running = false;
static buddy_proximity_t s_last_proximity = BUDDY_PROX_UNKNOWN;
static buddy_ble_conn_t *s_conn = NULL;
static uint16_t s_chr_profile_handle = 0;
static uint16_t s_chr_profile_write_handle = 0;

/* ── Chat link state ───────────────────────────────────────────── */
/* ATT MTU before negotiation.  Written as a literal because the NimBLE header
 * that defines BLE_ATT_MTU_DFLT is not part of this file's includes. */
#define BUDDY_ATT_MTU_DFLT  23

/* Preferred ATT MTU.  A chat frame tops out at BUDDY_CHAT_MSG_MAX, so the
 * payload limit is 200 + BUDDY_ATT_HEADER_LEN = 203 — anything from 247 up
 * carries the largest possible frame, and 247 is the usual value for a link
 * that only ever moves small frames.
 *
 * This used to be 512, which bought nothing: the frame is still clamped to 200
 * at buddy_ble_chat_max_len().
 *
 * ── The ACL buffer has to be at least this big ──────────────────
 * Raising the ATT MTU raises the size of the PDUs the *link layer* is then
 * allowed to hand to the controller, and the controller only ever sees
 * fragments of CONFIG_BT_NIMBLE_ACL_BUF_SIZE / TRANSPORT_ACL_SIZE bytes.  With
 * those at 64 while this is 247, every ATT response larger than a single
 * fragment stalls silently: both ends sit in the procedure until the ATT
 * timeout (~30 s) and neither reports anything, because no malformed packet is
 * ever seen — the reply simply never comes back.
 *
 * That is exactly what the numbers explain, and it is worth writing down
 * because the symptom is so misleading:
 *
 *   MTU exchange          3-byte response   -> 1 fragment  ✔ works
 *   Service walk          ATT error        -> 1 fragment  ✔ works
 *   GAP + GATT walk       2 attrs x 6 B    -> 44 B        ✔ works
 *   characteristic walk   3 attrs x 21 B   -> 71 B        ✘ 2 fragments
 *
 * Note the last three are all the *same* ATT operation (read-by-type): the
 * first two fit in one 64-byte fragment and completed, the third did not, and
 * it failed identically from a phone running nRF Connect and from this
 * firmware's own central.  So the fix belongs here in the config, not in the
 * discovery state machine: keep the ACL buffers >= this value.
 * sdkconfig.defaults.esp32s3 sets ACL_BUF_SIZE / TRANSPORT_ACL_SIZE to 251 for
 * exactly this reason. */
#define BUDDY_CHAT_ATT_MTU  247

/* Upper bound on how many handles the buddy service occupies, used to keep
 * characteristic discovery inside our own service instead of asking the peer
 * to walk its entire attribute table.  The service is one declaration plus
 * three characteristics (one of them with a CCCD) — 8 handles today. */
#define BUDDY_SVC_DISC_WINDOW  16

#define BUDDY_CONN_ITVL_UNITS(ms)       ((uint16_t)((ms) * 1000 / 1250))  /* 1.25 ms units */
#define BUDDY_CONN_TIMEOUT_UNITS(ms)    ((uint16_t)((ms) / 10))           /* 10 ms units   */

/* The two chat handles live in different ATT servers and must not share a
 * variable: on the central side we address the peer's characteristic, on the
 * peripheral side we notify from our own. */
static uint16_t s_peer_chat_handle = 0;       /* central: peer's chat value handle (write target) */
static uint16_t s_local_chat_handle = 0;      /* peripheral: our chat value handle (notify source) */
static uint16_t s_cccd_handle = 0;            /* central: peer's chat CCCD handle */
static uint16_t s_svc_start_handle = 0;       /* central: peer's buddy service start handle */
static uint16_t s_svc_end_handle = 0;         /* central: end of the range we search in it */
static bool     s_svc_found = false;          /* central: buddy service seen during the walk */
static uint16_t s_att_mtu = BUDDY_ATT_MTU_DFLT;
static bool     s_peer_subscribed = false;    /* peripheral: peer enabled notifications */
static bool     s_chat_link_up = false;       /* a chat path was established on this link */
static bool     s_chat_ready_sent = false;    /* BUDDY_CHAT_RX_LINK_READY already posted */

/* Advertising payload is rebuilt whenever the dynamic flags change. */
static uint8_t s_adv_mfg[18];

/* What the last peer profile read revealed about its wearer, kept for the chat
 * task: the profile event queue belongs to the contact task, and the link is
 * already down by the time a dialogue ends.  Written from the NimBLE host task,
 * read from the chat task, hence the lock. */
static char                s_peer_appearance[BUDDY_APPEARANCE_LEN];
static char                s_peer_belongings[BUDDY_BELONGINGS_LEN];
static bool                s_peer_profile_for_chat_valid = false;
static SemaphoreHandle_t   s_peer_profile_lock = NULL;

/* ── BLE UUIDs ─────────────────────────────────────────────────── */
static const ble_uuid128_t g_buddy_svc_uuid =
    BLE_UUID128_INIT(BUDDY_SVC_UUID);
static const ble_uuid128_t g_buddy_chr_profile_uuid =
    BLE_UUID128_INIT(BUDDY_CHR_PROFILE_UUID);
static const ble_uuid128_t g_buddy_chr_profile_write_uuid =
    BLE_UUID128_INIT(BUDDY_CHR_PROFILE_WRITE_UUID);
static const ble_uuid128_t g_buddy_chr_chat_uuid =
    BLE_UUID128_INIT(BUDDY_CHR_CHAT_UUID);

/* ── Forward declarations ──────────────────────────────────────── */
static int buddy_ble_gap_event(struct ble_gap_event *event, void *arg);
static int buddy_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static void buddy_ble_start_adv(void);
static void buddy_ble_start_scan(void);
static void gatt_start_read(uint16_t conn_handle);
static void gatt_start_chat_setup(uint16_t conn_handle);
static void gatt_start_discovery(uint16_t conn_handle);

/* When the last conversation finished, whatever peer it was with.
 *
 * The per-peer cool-down below is the nice version — it lets us go and talk to
 * somebody *else* immediately — but it depends on recognising the same peer
 * across two different spellings of its address, and getting that wrong means
 * no cool-down at all.  This global gate has no such dependency: after any
 * conversation, outbound connections stop for the cool-down window.  It is the
 * backstop that makes "two badges sitting next to each other" impossible to turn
 * into an endless conversation, even if peer identification fails. */
static int64_t s_last_session_end_ms = 0;

/* ── Peer tracking helpers ─────────────────────────────────────── */
static peer_track_t *peer_find_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) return &s_peers[i];
    }
    return NULL;
}

static peer_track_t *peer_find_or_add(const uint8_t *mac)
{
    peer_track_t *p = peer_find_by_mac(mac);
    if (p) return p;

    if (s_peer_count < PEER_TRACK_MAX) {
        p = &s_peers[s_peer_count++];
    } else {
        /* Evict the least useful entry — but never one that is still cooling
         * down from a conversation.
         *
         * This used to evict purely on "oldest advertisement", which quietly
         * destroyed the anti-rechat cooldown: in any room with a few dozen
         * advertisers the peer we just talked to was evicted within seconds,
         * its cooldown went with it, and the two badges reconnected and talked
         * another round, forever.  A cool-down entry is the only thing standing
         * between us and that loop, so it outranks everything here.
         *
         * If every slot is cooling down (unlikely — that needs 32 recent
         * partners) the entries that never connected are the safest to drop;
         * otherwise the oldest advertisement goes. */
        int64_t now = esp_timer_get_time() / 1000LL;
        int victim = -1;

        for (int i = 0; i < PEER_TRACK_MAX; i++) {
            if (s_peers[i].last_conn_ms > 0 &&
                (now - s_peers[i].last_conn_ms) < buddy_cooldown_peer_ms()) {
                continue;   /* still cooling down — protected */
            }
            if (victim < 0 ||
                s_peers[i].last_ad_ms < s_peers[victim].last_ad_ms) {
                victim = i;
            }
        }

        if (victim < 0) {
            /* Everything is protected; drop the oldest regardless. */
            victim = 0;
            for (int i = 1; i < PEER_TRACK_MAX; i++) {
                if (s_peers[i].last_ad_ms < s_peers[victim].last_ad_ms) victim = i;
            }
        }
        p = &s_peers[victim];
    }

    memset(p, 0, sizeof(*p));
    memcpy(p->mac, mac, 6);
    return p;
}

/* A conversation with `mac` has finished; start its cool-down.
 *
 * Recorded per peer id and looked up at the end of a session, because the
 * cooldown is about *this* pairing: talking to a different badge later should
 * not be delayed by it. */
void buddy_ble_note_round_complete(const char *peer_device_id)
{
    if (!peer_device_id || !peer_device_id[0] || !s_peers) return;

    /* The gate that does not depend on identifying the peer. */
    s_last_session_end_ms = esp_timer_get_time() / 1000LL;

    /* Match on the device id or on the BLE address.
     *
     * These are two different strings for the same peer, and confusing them is
     * what made this function silently do nothing: the profile's device id is
     * the WiFi MAC (esp_read_mac(ESP_MAC_WIFI_STA)) while the connection's
     * identity address is the BT MAC — two octets apart.  The peer entry may
     * have been created from either, so accept both. */
    for (int i = 0; i < s_peer_count; i++) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 s_peers[i].mac[0], s_peers[i].mac[1], s_peers[i].mac[2],
                 s_peers[i].mac[3], s_peers[i].mac[4], s_peers[i].mac[5]);

        if (strcmp(s_peers[i].device_id, peer_device_id) == 0 ||
            strcmp(mac_str, peer_device_id) == 0) {
            s_peers[i].last_conn_ms = esp_timer_get_time() / 1000LL;
            ESP_LOGI(TAG, "Cooling down %s for %lld s after this conversation",
                     peer_device_id, (long long)(buddy_cooldown_peer_ms() / 1000));
            return;
        }
    }

    /* Not fatal any more — the global gate above covers this peer regardless —
     * but it means per-peer tracking missed, which is worth knowing. */
    ESP_LOGW(TAG, "Conversation with %s ended but it is not in the peer table — "
                  "only the global cool-down applies", peer_device_id);
}

/* Why an advertisement should not become a connection, or NULL if it should.
 *
 * Returned as a string so the scan path can name the specific gate, including
 * the global one — "we are deliberately not re-chatting" and "the radio never
 * saw the peer" used to look identical in the log. */
static const char *peer_connect_reject_reason(const uint8_t *mac)
{
    int64_t now = esp_timer_get_time() / 1000LL;

    /* Global gate first: if we finished a conversation recently, do not open
     * another link to anyone, whatever the per-peer record says.  Peer identity
     * is derived from two different addresses (the WiFi MAC a badge reports in
     * its profile, and the BT address that the connection and the scan see), and
     * a mismatch there left this decision with no cool-down at all, which is how
     * the two badges came to talk round after round.  This branch cannot
     * mismatch — and it is deliberately checked *before* the unknown-peer
     * shortcut below, which would otherwise let the very first advertisement
     * after a session straight through. */
    if (s_last_session_end_ms > 0 &&
        (now - s_last_session_end_ms) < buddy_cooldown_session_ms()) {
        return "session cooldown";
    }

    peer_track_t *p = peer_find_by_mac(mac);
    if (!p) return NULL;

    if (p->blocked) return "blocked peer";

    /* Per-peer record, which is what lets us talk to somebody *else* while this
     * one is still cooling down. */
    if (p->last_conn_ms > 0 && (now - p->last_conn_ms) < buddy_cooldown_peer_ms()) {
        return "peer cooldown";
    }

    /* 2-second dedup.  Reads BUDDY_BEACON_DEDUP_MS rather than a literal: the
     * constant existed but nothing used it, so the number here was the real
     * setting and the one in buddy.h was decoration. */
    if (p->last_ad_ms > 0 && (now - p->last_ad_ms) < BUDDY_BEACON_DEDUP_MS) {
        return "advertisement dedup";
    }

    return NULL;
}

/* ── Advertising setup ─────────────────────────────────────────── */

/* Flags go out in the last byte of the manufacturer data.  Everything here is
 * recomputed on every (re)build so a stale NET_OK cannot outlive a WiFi drop. */
static uint8_t buddy_ble_current_flags(void)
{
    uint8_t flags = BUDDY_FLAG_ACCEPTING | BUDDY_FLAG_CHAT_CAPABLE;

    if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) flags |= BUDDY_FLAG_PRIVACY;
    /* Deliberately not gated on having a push target: a badge that can only
     * observe still takes part, and the peer that can push relays for both. */
    if (wifi_manager_is_connected()) flags |= BUDDY_FLAG_NET_OK;

    return flags;
}

static void buddy_ble_build_adv_fields(struct ble_hs_adv_fields *fields)
{
    const buddy_identity_t *id = buddy_identity_get();
    uint8_t dev_id_bytes[6] = {0};
    sscanf(id->device_id, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &dev_id_bytes[0], &dev_id_bytes[1], &dev_id_bytes[2],
           &dev_id_bytes[3], &dev_id_bytes[4], &dev_id_bytes[5]);

    /* Read straight from NVS: the hash is stored on its own, so building the
     * advertising payload no longer means materialising the whole profile. */
    uint8_t profile_hash[8] = {0};
    buddy_profile_get_hash(profile_hash);

    /* company_id(2) + version(1) + device_id(6) + hash(8) + flags(1) */
    s_adv_mfg[0] = BUDDY_MFG_COMPANY_ID & 0xFF;
    s_adv_mfg[1] = (BUDDY_MFG_COMPANY_ID >> 8) & 0xFF;
    s_adv_mfg[2] = BUDDY_PROTO_VERSION;
    memcpy(&s_adv_mfg[3], dev_id_bytes, 6);
    memcpy(&s_adv_mfg[9], profile_hash, 8);
    s_adv_mfg[17] = buddy_ble_current_flags();

    memset(fields, 0, sizeof(*fields));
    fields->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields->mfg_data = s_adv_mfg;
    fields->mfg_data_len = sizeof(s_adv_mfg);
}

/* Human-readable name, for someone scanning with a phone rather than for the
 * badge-to-badge protocol — that one keys off the manufacturer data.
 *
 * Derived from the *chip's* WiFi MAC, which is exactly the value a new device
 * identity is generated from (buddy_profile.c: esp_read_mac(ESP_MAC_WIFI_STA)).
 * It deliberately does not read the stored identity blob: that blob is written
 * once at first boot and persists, so a badge whose identity predates a change
 * in how the id is derived advertises a name that matches nothing — which is how
 * "MagicBadge-E830" came to be running with a device id of ...:79:58, and why
 * an earlier cool-down keyed on identity could never find the peer it had just
 * talked to. */
#define BUDDY_BLE_NAME_PREFIX "MagicBadge-"

static char s_adv_name[sizeof(BUDDY_BLE_NAME_PREFIX) + 4];

static const char *buddy_ble_adv_name(void)
{
    static bool resolved = false;

    /* on_sync() asks for the name unconditionally, so it has to be valid the
     * first time it is called — hence the cache, filled once. */
    if (!resolved) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_adv_name, sizeof(s_adv_name),
                 BUDDY_BLE_NAME_PREFIX "%02X%02X", mac[4], mac[5]);
        resolved = true;
    }

    return s_adv_name;
}

static void buddy_ble_start_adv(void)
{
    if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) {
        ESP_LOGI(TAG, "Privacy mode — advertising suppressed");
        return;
    }

    struct ble_hs_adv_fields fields;
    buddy_ble_build_adv_fields(&fields);
    ble_gap_adv_set_fields(&fields);

    /* Name goes in the scan response, not the advertising packet: the flags
     * (3 bytes) plus the manufacturer data (20) already fill 23 of the legacy
     * 31-byte budget, and "MagicBadge-XXXX" needs 17 more.  The scan response
     * has its own 31 bytes, so a phone doing an active scan gets the name while
     * our own payload stays untouched.  Nothing in our scan path reads it —
     * buddy_ble_gap_event() acts only on packets carrying our manufacturer
     * data and ignores the rest. */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.name = (const uint8_t *)buddy_ble_adv_name();
    rsp_fields.name_len = strlen((const char *)rsp_fields.name);
    rsp_fields.name_is_complete = 1;
    int rsp_rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rsp_rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields failed: %d", rsp_rc);
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(BUDDY_BLE_ADV_PERIOD_MS - BUDDY_BLE_ADV_JITTER_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(BUDDY_BLE_ADV_PERIOD_MS + BUDDY_BLE_ADV_JITTER_MS),
        .channel_map = 0x07,
    };

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, buddy_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started as \"%s\" (interval %d-%dms, flags=0x%02x)",
                 buddy_ble_adv_name(),
                 BUDDY_BLE_ADV_PERIOD_MS - BUDDY_BLE_ADV_JITTER_MS,
                 BUDDY_BLE_ADV_PERIOD_MS + BUDDY_BLE_ADV_JITTER_MS,
                 s_adv_mfg[17]);
    }
}

void buddy_ble_refresh_adv_flags(void)
{
    if (!s_running || !ble_hs_synced()) return;

    if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) {
        ble_gap_adv_stop();
        return;
    }

    /* Adv fields are read-only while an advertising instance is running, so
     * bounce it: stop, republish the payload, start again. */
    bool was_adv = ble_gap_adv_active() != 0;
    uint8_t old_flags = s_adv_mfg[17];

    if (was_adv) {
        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    struct ble_hs_adv_fields fields;
    buddy_ble_build_adv_fields(&fields);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    /* Safe to call unconditionally: if it was running we just stopped it. */
    buddy_ble_start_adv();

    if (old_flags != s_adv_mfg[17]) {
        ESP_LOGI(TAG, "Adv flags 0x%02x -> 0x%02x", old_flags, s_adv_mfg[17]);
    }
}

/* ── Scanning ──────────────────────────────────────────────────── */
static void buddy_ble_start_scan(void)
{
    struct ble_gap_disc_params scan_params = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(BUDDY_BLE_SCAN_INTERVAL_MS),
        .window = BLE_GAP_SCAN_WIN_MS(BUDDY_BLE_SCAN_WINDOW_MS),
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,          /* active scanning */
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                          &scan_params, buddy_ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else if (rc == 0) {
        ESP_LOGI(TAG, "Scanning started (interval=%dms window=%dms)",
                 BUDDY_BLE_SCAN_INTERVAL_MS, BUDDY_BLE_SCAN_WINDOW_MS);
    }
}

/* ── Profile helpers ───────────────────────────────────────────── */
/* What this badge hands another device over BLE.
 *
 * Only what someone standing in front of you could see for themselves: how their
 * wearer looks and what they are carrying.  Name and bio are deliberately *not*
 * here — you do not know a stranger's name or history by looking at them, and
 * that is what the conversation is for.  It also means this characteristic,
 * which is BLE_GATT_CHR_F_READ with no pairing, authentication or encryption,
 * cannot be scraped for identities or personal histories by any device that
 * comes within range and connects.
 *
 * Runs on the NimBLE host task, whose stack is measured in a few kilobytes — so
 * nothing larger than a pointer goes on the stack here.  The field buffers live
 * in PSRAM and cJSON keeps its own heap copies of the strings. */
static int serialize_profile(char *buf, size_t size)
{
    const buddy_identity_t *id = buddy_identity_get();
    cJSON *root = NULL;
    char *appearance = NULL;
    char *belongings = NULL;
    char *json = NULL;
    int len = -1;

    appearance = heap_caps_calloc(1, BUDDY_APPEARANCE_LEN, MALLOC_CAP_SPIRAM);
    belongings = heap_caps_calloc(1, BUDDY_BELONGINGS_LEN, MALLOC_CAP_SPIRAM);
    if (!appearance || !belongings) goto done;

    /* Read the fields straight from NVS — one open, no intermediate struct. */
    const char *keys[] = { BUDDY_PROF_KEY_APPEARANCE, BUDDY_PROF_KEY_BELONGINGS };
    char *outs[] = { appearance, belongings };
    const size_t sizes[] = { BUDDY_APPEARANCE_LEN, BUDDY_BELONGINGS_LEN };
    buddy_profile_get_fields(keys, outs, sizes, 2);

    root = cJSON_CreateObject();
    if (!root) goto done;

    cJSON_AddStringToObject(root, "ap", appearance);
    cJSON_AddStringToObject(root, "bl", belongings);
    cJSON_AddStringToObject(root, "did", id->device_id);

    json = cJSON_PrintUnformatted(root);
    if (!json) goto done;

    len = (int)strlen(json);
    if ((size_t)len >= size) len = (int)size - 1;
    memcpy(buf, json, len);
    buf[len] = '\0';

done:
    free(json);
    if (root) cJSON_Delete(root);
    heap_caps_free(appearance);
    heap_caps_free(belongings);
    return len;
}

/* Pull the visible half out of a peer's profile JSON.
 *
 * A peer running older firmware may still send "dn" and "bi"; those are ignored —
 * this badge learns a name from the person saying it out loud, not from a field
 * another badge volunteered.  Writes only into the caller's buffers, never into
 * a shared struct, so the caller decides where the memory comes from. */
static void parse_peer_profile_into(const char *json,
                                    char *appearance, size_t appearance_size,
                                    char *belongings, size_t belongings_size)
{
    if (appearance && appearance_size) appearance[0] = '\0';
    if (belongings && belongings_size) belongings[0] = '\0';
    if (!json) return;

    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *ap = cJSON_GetObjectItem(root, "ap");
    cJSON *bl = cJSON_GetObjectItem(root, "bl");

    if (ap && cJSON_IsString(ap) && appearance && appearance_size)
        snprintf(appearance, appearance_size, "%s", ap->valuestring);
    if (bl && cJSON_IsString(bl) && belongings && belongings_size)
        snprintf(belongings, belongings_size, "%s", bl->valuestring);

    cJSON_Delete(root);
}

/* ── Event posting ─────────────────────────────────────────────── */
static void post_profile_event(const uint8_t *peer_mac, const char *device_id,
                               int8_t rssi, const char *profile_json)
{
    /* On the heap, not the stack: this carries two full character fields, and
     * the caller is the NimBLE host task, whose stack is only a few kilobytes.
     * (A local `buddy_event_t` here is what overflowed it once already.) */
    buddy_event_t *evt = heap_caps_calloc(1, sizeof(*evt), MALLOC_CAP_SPIRAM);
    if (!evt) {
        ESP_LOGW(TAG, "No memory for a profile event");
        return;
    }
    evt->type = BUDDY_EVT_PROFILE_READY;

    /* Parse straight into the event, which lives in PSRAM.
     *
     * There used to be two BUDDY_*_LEN (1 KB each) locals here holding the
     * parsed fields before copying them into the event.  This runs on the NimBLE
     * host task, whose stack is CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE — 4096
     * bytes by default — so two kilobytes of it was over half the task's stack,
     * and the overflow fired on every disconnect (which is one of the two places
     * that calls this).  The badge then rebooted, re-advertised, was found again
     * and started another conversation: an endless loop that looked like the
     * chat failing to stop. */
    if (profile_json) {
        parse_peer_profile_into(profile_json,
                                evt->peer_appearance, sizeof(evt->peer_appearance),
                                evt->peer_belongings, sizeof(evt->peer_belongings));
    }

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             peer_mac[0], peer_mac[1], peer_mac[2],
             peer_mac[3], peer_mac[4], peer_mac[5]);
    strncpy(evt->peer_device_id, device_id[0] ? device_id : mac_str,
            sizeof(evt->peer_device_id) - 1);

    memcpy(evt->peer_mac, peer_mac, 6);
    evt->rssi = rssi;
    evt->proximity = buddy_proximity_classify();
    evt->peer_profile_valid = (evt->peer_appearance[0] || evt->peer_belongings[0]);

    /* Keep a copy for the chat task.  The event queue is consumed by the contact
     * task, so a dialogue cannot read it from there; and the link (with the peer
     * it describes) is gone by the time a session ends. */
    if (evt->peer_profile_valid) {
        xSemaphoreTake(s_peer_profile_lock, portMAX_DELAY);
        memcpy(s_peer_appearance, evt->peer_appearance, sizeof(s_peer_appearance));
        memcpy(s_peer_belongings, evt->peer_belongings, sizeof(s_peer_belongings));
        s_peer_profile_for_chat_valid = true;
        xSemaphoreGive(s_peer_profile_lock);
    }

    if (xQueueSend(s_event_queue, evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping profile from %s", evt->peer_device_id);
    }
    heap_caps_free(evt);
}

/* What the peer's badge said about its wearer, for the dialogue to use.  Only
 * the visible half is ever stored, so these are the only outputs. */
bool buddy_ble_peer_profile(char *appearance, size_t appearance_size,
                            char *belongings, size_t belongings_size)
{
    if (!s_peer_profile_lock) return false;

    xSemaphoreTake(s_peer_profile_lock, portMAX_DELAY);
    bool ok = s_peer_profile_for_chat_valid;
    if (ok) {
        if (appearance && appearance_size)
            snprintf(appearance, appearance_size, "%s", s_peer_appearance);
        if (belongings && belongings_size)
            snprintf(belongings, belongings_size, "%s", s_peer_belongings);
    }
    xSemaphoreGive(s_peer_profile_lock);
    return ok;
}

/* Forget the peer profile: called when a link comes up, so a dialogue can never
 * describe the person it met last time. */
void buddy_ble_clear_peer_profile(void)
{
    if (!s_peer_profile_lock) return;

    xSemaphoreTake(s_peer_profile_lock, portMAX_DELAY);
    s_peer_profile_for_chat_valid = false;
    s_peer_appearance[0] = '\0';
    s_peer_belongings[0] = '\0';
    xSemaphoreGive(s_peer_profile_lock);
}

/* ── Chat transport plumbing ───────────────────────────────────── */

/* Called from the NimBLE host task, so it must never block: the payload is
 * copied into PSRAM and the consumer owns the copy. */
static void chat_post(buddy_chat_rx_kind_t kind, const uint8_t *data, size_t len,
                      int8_t rssi)
{
    if (!s_chat_queue) return;

    buddy_chat_rx_t rx = {0};
    rx.kind = kind;
    rx.rssi = rssi;

    if (kind == BUDDY_CHAT_RX_DATA) {
        if (!data || len == 0) return;
        rx.text = heap_caps_calloc(1, len + 1, MALLOC_CAP_SPIRAM);
        if (!rx.text) {
            ESP_LOGW(TAG, "No memory for chat frame (%u bytes)", (unsigned)len);
            return;
        }
        memcpy(rx.text, data, len);
        rx.len = len;
    }

    if (xQueueSend(s_chat_queue, &rx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Chat queue full, dropping %s",
                 kind == BUDDY_CHAT_RX_DATA ? "frame" : "link event");
        if (rx.text) heap_caps_free(rx.text);
    }
}

/* The two roles reach "ready" at different moments: the central once it has
 * subscribed, the peripheral once the subscription request arrives.  Whichever
 * happens first announces the link, and only once. */
static void chat_post_link_ready(void)
{
    if (s_chat_ready_sent) return;
    s_chat_ready_sent = true;
    s_chat_link_up = true;
    ESP_LOGI(TAG, "Chat link ready (role=%s, mtu=%u)",
             s_conn->outgoing ? "central" : "peripheral", (unsigned)s_att_mtu);
    chat_post(BUDDY_CHAT_RX_LINK_READY, NULL, 0, s_conn->rssi);
}

/* Ask for a short interval and a 4 s supervision timeout.  The timeout doubles
 * as a "the owner walked away" detector, so it is deliberately tight. */
static void chat_apply_conn_params(uint16_t conn_handle)
{
    struct ble_gap_upd_params params = {
        .itvl_min = BUDDY_CONN_ITVL_UNITS(BUDDY_CONN_ITVL_MS),
        .itvl_max = BUDDY_CONN_ITVL_UNITS(BUDDY_CONN_ITVL_MS),
        .latency = BUDDY_CONN_LATENCY,
        .supervision_timeout = BUDDY_CONN_TIMEOUT_UNITS(BUDDY_CONN_TIMEOUT_MS),
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "Connection param update rejected: %d", rc);
    }
}

/* ── GATT client: discover and exchange profiles ────────────────── */
/* The write is sent in chunks rather than as one long write.
 *
 * A profile carrying whole appearance and belongings descriptions runs to
 * several hundred bytes, which is far more than one ATT write can hold
 * (MTU - 3 = 244 bytes here).  "Write Long" is the textbook answer and is what
 * this used first, but it failed against our own peripheral: the peer's ATT
 * server answered BLE_ATT_ERR_INVALID_OFFSET (7) during the Prepare/Execute
 * sequence, and the exchange died.  Rather than depend on a long-write
 * implementation on both ends, the value is split into plain writes that each
 * fit the link; the receiver accumulates them and parses on the final chunk.
 *
 * Format per chunk: one marker byte ('S' more to come, 'E' last), then payload.
 * A chunk is at most BUDDY_PROF_CHUNK_MAX header-inclusive, so no chunk can ever
 * exceed the negotiated MTU. */
static char   *s_profile_tx = NULL;   /* whole JSON, PSRAM, freed when sent */
static size_t  s_profile_tx_len = 0;
static size_t  s_profile_tx_off = 0;

/* Set while reading the peer's profile: true once the final chunk has been seen. */
static bool    s_profile_rx_done = false;

/* Offset the peer last asked us for, while it reads our profile in windows. */
static uint16_t s_profile_read_offset = 0;

/* The two call each other: a chunk is sent, its completion callback starts the
 * next one. */
static int gatt_profile_write_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg);

static void gatt_profile_send_chunk(uint16_t conn_handle)
{
    if (!s_chr_profile_write_handle || !s_profile_tx) {
        s_conn->profile_sent = true;
        gatt_start_read(conn_handle);
        return;
    }

    size_t left = s_profile_tx_len - s_profile_tx_off;
    bool last = left <= (BUDDY_PROF_CHUNK_MAX - 1);
    size_t take = last ? left : (BUDDY_PROF_CHUNK_MAX - 1);

    char *chunk = heap_caps_calloc(1, take + 1, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        ESP_LOGW(TAG, "No memory for a profile chunk");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    chunk[0] = last ? CHAT_CHUNK_LAST : CHAT_CHUNK_MORE;
    memcpy(chunk + 1, s_profile_tx + s_profile_tx_off, take);
    s_profile_tx_off += take;

    int rc = ble_gattc_write_flat(conn_handle, s_chr_profile_write_handle,
                                  chunk, (uint16_t)(take + 1),
                                  gatt_profile_write_cb, NULL);
    heap_caps_free(chunk);
    if (rc != 0) {
        ESP_LOGW(TAG, "Profile chunk write failed to start: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int gatt_profile_write_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Profile write error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (s_profile_tx && s_profile_tx_off < s_profile_tx_len) {
        /* More to send — one write at a time, since NimBLE runs a single ATT
         * client procedure at a time anyway. */
        gatt_profile_send_chunk(conn_handle);
        return 0;
    }

    ESP_LOGI(TAG, "Profile write complete (%u bytes)", (unsigned)s_profile_tx_len);
    s_conn->profile_sent = true;
    if (s_profile_tx) {
        heap_caps_free(s_profile_tx);
        s_profile_tx = NULL;
    }

    /* Write done — now start the read (serialized to avoid proc limit) */
    gatt_start_read(conn_handle);

    return 0;
}

/* Read the peer's profile in chunks, by the same scheme the write direction uses.
 *
 * ble_gattc_read_long() was the first attempt and the peer's ATT server answered
 * its Read Blob request with BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN, exactly as the
 * long write had answered INVALID_OFFSET.  Both directions of "long" ATT traffic
 * are evidently unreliable between two copies of this firmware, so neither is
 * used: the client asks for a window by writing a small command to the profile
 * characteristic, and reads that window back. */
static int  gatt_profile_chunk_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg);
static int  gatt_profile_cmd_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg);
static void gatt_profile_write_cmd(uint16_t conn_handle, uint16_t offset);

static void gatt_profile_done(uint16_t conn_handle)
{
    if (s_conn->profile_len == 0) {
        ESP_LOGW(TAG, "Peer profile came back empty");
    } else if (s_conn->peer_flags & BUDDY_FLAG_CHAT_CAPABLE) {
        /* A peer we will hold a conversation with: keep the link and open the
         * chat path.  The exchange had to finish first, because the dialogue
         * speaks about whoever is standing there, and this is the only moment
         * that information is on the wire. */
        ESP_LOGI(TAG, "Profile exchange complete, opening chat link...");
        gatt_start_chat_setup(conn_handle);
        return;
    } else if (s_conn->profile_sent) {
        /* Nothing further to do with this one. */
        ESP_LOGI(TAG, "Profile exchange complete, disconnecting");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    s_conn->profile_len = 0;
}

static int gatt_profile_chunk_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Profile read error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    /* A successful plain read calls back exactly once, with the value in attr.
     * There is no "end of procedure" call: NimBLE only passes attr == NULL on
     * the error and timeout paths (see ble_gattc_read_cb(), which asserts
     * `attr != NULL || status != 0`).  So the next window has to be requested
     * from here — an earlier version waited for a second call that never comes,
     * and the exchange stalled after the first window. */
    if (!attr || !attr->om) {
        ESP_LOGW(TAG, "Profile read returned no data (status=%d)",
                 error ? error->status : 0);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    uint16_t om_len = OS_MBUF_PKTLEN(attr->om);
    if (om_len < 1) {
        ESP_LOGW(TAG, "Profile read returned an empty window");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    uint8_t marker = 0;
    os_mbuf_copydata(attr->om, 0, 1, &marker);
    s_profile_rx_done = (marker == CHAT_CHUNK_LAST);

    ESP_LOGI(TAG, "Profile read window: %u bytes, marker='%c'", (unsigned)om_len,
             (char)marker);

    size_t payload = om_len - 1;
    size_t space = sizeof(s_conn->profile_buf) - 1 - s_conn->profile_len;
    if (payload > space) {
        ESP_LOGW(TAG, "Peer profile exceeds %u bytes, truncated",
                 (unsigned)(sizeof(s_conn->profile_buf) - 1));
        payload = space;
        s_profile_rx_done = true;
    }
    if (payload > 0) {
        os_mbuf_copydata(attr->om, 1, payload,
                         s_conn->profile_buf + s_conn->profile_len);
        s_conn->profile_len += payload;
    }
    s_conn->profile_buf[s_conn->profile_len] = '\0';

    ESP_LOGI(TAG, "Profile read window done: collected %u bytes, %s",
             (unsigned)s_conn->profile_len,
             s_profile_rx_done ? "complete" : "asking for the next window");

    if (s_profile_rx_done) {
        gatt_profile_done(conn_handle);
    } else {
        gatt_profile_write_cmd(conn_handle, (uint16_t)s_conn->profile_len);
    }
    return 0;
}

/* Ask the peer for the window starting at `offset`. */
static void gatt_profile_write_cmd(uint16_t conn_handle, uint16_t offset)
{
    uint8_t cmd[3] = { PROFILE_CMD_GET, (uint8_t)(offset & 0xFF),
                       (uint8_t)((offset >> 8) & 0xFF) };

    /* s_profile_rx_done is deliberately NOT cleared here: this runs once per
     * window, and clearing it would erase the "last window seen" state that
     * gatt_profile_chunk_cb just set — the read would never terminate.  It is
     * reset once in gatt_start_read(), at the start of the whole exchange. */

    ESP_LOGI(TAG, "Profile read: requesting window at offset %u", (unsigned)offset);

    int rc = ble_gattc_write_flat(conn_handle, s_chr_profile_write_handle,
                                  cmd, sizeof(cmd), gatt_profile_cmd_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Profile read command failed to start: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int gatt_profile_cmd_cb(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Profile read command rejected: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    ESP_LOGI(TAG, "Profile read command accepted, reading the window...");

    int rc = ble_gattc_read(conn_handle, s_chr_profile_handle,
                            gatt_profile_chunk_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Profile chunk read failed to start: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int gatt_chr_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);

static int gatt_svc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg);

static void gatt_start_exchange(uint16_t conn_handle)
{
    if (s_profile_tx) {                 /* a previous exchange never finished */
        heap_caps_free(s_profile_tx);
        s_profile_tx = NULL;
    }

    s_profile_tx = heap_caps_calloc(1, BUDDY_PROFILE_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_profile_tx) {
        ESP_LOGE(TAG, "Failed to allocate own profile buffer");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    int own_len = serialize_profile(s_profile_tx, BUDDY_PROFILE_MAX_BYTES);
    if (own_len < 0) {
        ESP_LOGE(TAG, "Failed to serialize own profile");
        heap_caps_free(s_profile_tx);
        s_profile_tx = NULL;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    s_profile_tx_len = (size_t)own_len;
    s_profile_tx_off = 0;
    gatt_profile_send_chunk(conn_handle);
}

static void gatt_start_read(uint16_t conn_handle)
{
    if (!s_chr_profile_handle || !s_chr_profile_write_handle) {
        ESP_LOGW(TAG, "Profile characteristics not found, disconnecting");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    /* Kick off the windowed read: our profile has already been sent, now pull
     * theirs one window at a time (see gatt_profile_chunk_cb). */
    s_conn->profile_len = 0;
    s_profile_rx_done = false;
    gatt_profile_write_cmd(conn_handle, 0);
}

/* ── GATT client: bring up the chat path ───────────────────────── */

static int gatt_cccd_write_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0) {
        ESP_LOGW(TAG, "CCCD write failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    ESP_LOGI(TAG, "Subscribed to peer notifications");
    chat_post_link_ready();
    return 0;
}

static int gatt_chat_write_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        /* The session layer owns retransmission — surface the failure and move on.
         *
         * Say which failure it is.  When the link is already being torn down the
         * controller reports BLE_HS_ENOMEM (7), which reads exactly like "the
         * board ran out of memory" but means "there is no link left to write
         * on" — the usual case here, since the closing frame races the
         * disconnect that follows it. */
        if (!s_conn || !s_conn->active) {
            ESP_LOGI(TAG, "Chat write abandoned: the link is already down "
                          "(status=%d)", error->status);
        } else {
            ESP_LOGW(TAG, "Chat write failed: status=%d", error->status);
        }
    }
    return 0;
}

static int gatt_dsc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Descriptor discovery error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (!dsc) {
        /* Descriptors exhausted — subscribe, or give up on the link. */
        if (!s_cccd_handle) {
            ESP_LOGW(TAG, "Chat CCCD not found, disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        uint8_t notify_on[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn_handle, s_cccd_handle,
            notify_on, sizeof(notify_on), gatt_cccd_write_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "CCCD subscribe failed: %d", rc);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_cccd_handle = dsc->handle;
        ESP_LOGI(TAG, "Found chat CCCD: handle=0x%04x", s_cccd_handle);
    }
    return 0;
}

static void gatt_start_chat_setup(uint16_t conn_handle)
{
    if (!s_peer_chat_handle) {
        ESP_LOGW(TAG, "Peer advertises chat but has no chat characteristic");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    s_cccd_handle = 0;
    int rc = ble_gattc_disc_all_dscs(conn_handle, s_peer_chat_handle,
                                     s_svc_end_handle, gatt_dsc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Descriptor discovery failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* ── GATT client: start-of-link sequence ───────────────────────── */
/* NimBLE runs one GATT client procedure at a time, so every step of the
 * bring-up chain is started from the previous step's completion callback
 * rather than fired back-to-back.  Order: MTU -> services -> characteristics
 * -> descriptors -> CCCD subscribe. */

static void gatt_start_discovery(uint16_t conn_handle)
{
    int rc = ble_gattc_disc_all_svcs(conn_handle, gatt_svc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Service discovery failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int gatt_mtu_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       uint16_t mtu, void *arg)
{
    if (error && error->status != 0) {
        ESP_LOGW(TAG, "MTU exchange failed: status=%d", error->status);
    } else if (mtu > 0) {
        /* Logged at DEBUG only: BLE_GAP_EVENT_MTU reports the same result on
         * both roles, and this callback runs on top of it, so an INFO line here
         * printed every negotiation twice. */
        s_att_mtu = mtu;
        ESP_LOGD(TAG, "MTU exchange reported %u", (unsigned)mtu);
    }

    /* Carry on with discovery even if the exchange failed — the 23-byte
     * default still carries a short chat frame. */
    gatt_start_discovery(conn_handle);
    return 0;
}

static int gatt_svc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg)
{
    /* NimBLE delivers final callback with non-NULL error even on success;
     * BLE_HS_EDONE (14) means discovery complete, not an error. */
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Service discovery error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (!service) {
        /* Terminating callback: the service walk has finished, so this is the
         * first moment at which the next GATT client procedure may be started.
         *
         * This callback also fires once per service *while that walk is still
         * running*.  Starting characteristic discovery from there allocates a
         * second procedure whose read-by-type request has to queue behind the
         * walk's own outstanding request; when the walk then completes, nothing
         * kicks the queued request again and it is never transmitted.  The peer
         * sees no ATT traffic at all, and we sit here until the procedure times
         * out — a silent 30 s, which is exactly what the hardware log shows. */
        if (!s_svc_found) {
            ESP_LOGW(TAG, "Buddy service not found, disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        s_chr_profile_handle = 0;
        s_chr_profile_write_handle = 0;
        s_peer_chat_handle = 0;

        /* State the link's frame size at the moment the ATT client procedure is
         * about to start.  The ATT procedure timeout that follows has two very
         * different causes — the request never left, or the reply never arrived
         * — and this is the cheapest way to see which MTU the exchange below is
         * actually operating at. */
        ESP_LOGI(TAG, "Service walk complete, discovering characteristics in 0x%04x..0x%04x (att_mtu=%u)...",
                 s_svc_start_handle, s_svc_end_handle, (unsigned)s_att_mtu);

        int rc = ble_gattc_disc_all_chrs(conn_handle, s_svc_start_handle,
                                         s_svc_end_handle,
                                         gatt_chr_disc_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Characteristic discovery failed to start: %d", rc);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    /* Log every service found for debugging */
    ESP_LOGI(TAG, "Svc found: handle=0x%04x..0x%04x type=%d",
             service->start_handle, service->end_handle,
             (int)service->uuid.u.type);

    /* Match our buddy service UUID */
    if (ble_uuid_cmp(&service->uuid.u, &g_buddy_svc_uuid.u) != 0) {
        return 0;  /* not our service, skip */
    }

    /* Found buddy service — remember where it lives and wait for the walk to
     * finish.  Nothing else may be started from here; see the terminating
     * branch above. */
    ESP_LOGI(TAG, "Buddy service matched (handle=0x%04x..0x%04x)",
             service->start_handle, service->end_handle);

    /* NimBLE reports the end handle as 0xFFFF whenever the service is the last
     * one in the peer's attribute table — which is the normal case here, since
     * a badge serves exactly one custom service.  Walking 1..0xFFFF makes the
     * peer's ATT server scan its whole table and makes the procedure unable to
     * finish on the "reached the end handle" path; it can then only terminate
     * via ATT_ERR_ATTR_NOT_FOUND.  Keep the search inside our own service. */
    uint16_t window_end = service->start_handle + BUDDY_SVC_DISC_WINDOW - 1;
    s_svc_start_handle = service->start_handle;
    s_svc_end_handle = (service->end_handle < window_end)
                     ? service->end_handle : window_end;
    if (s_svc_end_handle < service->start_handle) {
        s_svc_end_handle = service->start_handle;
    }
    s_svc_found = true;
    return 0;
}

static int gatt_chr_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        if (error->status == BLE_HS_ETIMEOUT) {
            /* Distinct from every other failure here: the peer completed the
             * connection, the MTU exchange and the service walk, then sent
             * nothing for the whole ATT procedure timeout (~30 s).  A
             * peripheral that simply had no matching characteristic would have
             * answered ATT_ERR_ATTR_NOT_FOUND immediately, so a timeout means
             * its ATT server never replied at all — its host task was starved,
             * or it could not build a response.  Look at the *peer's* log for
             * this window; ours cannot show either cause. */
            ESP_LOGW(TAG, "Characteristic discovery timed out: peer's ATT server "
                          "answered service discovery but sent no reply to "
                          "read-by-type (window 0x%04x..0x%04x)",
                     s_svc_start_handle, s_svc_end_handle);
        } else {
            ESP_LOGW(TAG, "Characteristic discovery error: status=%d", error->status);
        }
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (!chr) {
        /* Everyone exchanges profiles first, chat-capable or not.  The exchange
         * is how the other side learns who it is dealing with — and for a peer we
         * will hold a conversation with, it is what lets that dialogue mention
         * the person in front of it.  The chat link opens from the exchange's
         * completion callback rather than here, so the two never overlap on the
         * one ATT client procedure NimBLE allows at a time. */
        if (s_chr_profile_handle == 0 && s_chr_profile_write_handle == 0) {
            ESP_LOGW(TAG, "Buddy characteristics not found, disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "Characteristic discovery complete (profile read=0x%04x write=0x%04x, "
                      "chat=0x%04x), exchanging profiles...",
                 s_chr_profile_handle, s_chr_profile_write_handle, s_peer_chat_handle);
        gatt_start_exchange(conn_handle);
        return 0;
    }

    if (ble_uuid_cmp(&chr->uuid.u, &g_buddy_chr_chat_uuid.u) == 0) {
        s_peer_chat_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found chat characteristic: handle=0x%04x", s_peer_chat_handle);
    } else if (ble_uuid_cmp(&chr->uuid.u, &g_buddy_chr_profile_uuid.u) == 0) {
        s_chr_profile_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found profile read characteristic: handle=0x%04x", s_chr_profile_handle);
    } else if (ble_uuid_cmp(&chr->uuid.u, &g_buddy_chr_profile_write_uuid.u) == 0) {
        s_chr_profile_write_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found profile write characteristic: handle=0x%04x", s_chr_profile_write_handle);
    }
    return 0;
}

/* Record who is on the other end.
 *
 * The scan path fills these in from the peer's advertisement, which only ever
 * happens when *we* initiate.  A peripheral has no advertisement of its own to
 * learn from, so on an incoming connection these stayed empty — and since
 * buddy_ble_peer_id() derives "is the link alive" from peer_device_id, a live
 * link looked dead the moment anything asked.  Any role must be able to name
 * its peer, so derive it here from the connection itself, where the identity
 * address is known either way. */
static void buddy_ble_note_peer(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;

    if (ble_gap_conn_find(conn_handle, &desc) != 0) return;

    memcpy(s_conn->peer_mac, desc.peer_id_addr.val, 6);
    snprintf(s_conn->peer_device_id, sizeof(s_conn->peer_device_id),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             desc.peer_id_addr.val[0], desc.peer_id_addr.val[1],
             desc.peer_id_addr.val[2], desc.peer_id_addr.val[3],
             desc.peer_id_addr.val[4], desc.peer_id_addr.val[5]);

    /* Record the link against this peer, on both roles.
     *
     * This is what stops two badges from re-chatting forever, and until now only
     * the *central* ever recorded anything (in the scan handler).  A peripheral
     * learned its peer's identity here and then recorded nothing at all, so its
     * side had no suppression whatsoever: as soon as the session tore the link
     * down it immediately connected again, and the pair talked round after
     * round.
     *
     * The key has to be the BLE identity address rather than the profile's
     * device id, because that is what the scan path keys on and what shows up
     * again in the next connection request.  buddy_ble_note_round_complete()
     * covers the other spelling of the same peer. */
    if (s_peers) {
        peer_track_t *p = peer_find_or_add(desc.peer_id_addr.val);
        p->last_conn_ms = esp_timer_get_time() / 1000LL;
    }
}

/* ── NimBLE GAP event handler ──────────────────────────────────── */
static int buddy_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *d = &event->disc;
        int8_t rssi = d->rssi;

        /* Extract manufacturer data */
        uint8_t dev_id[6] = {0};
        uint8_t prof_hash[8] = {0};
        uint8_t mfg_flags = 0;
        bool is_buddy = false;

        /* Parse advertising data fields */
        const uint8_t *ad = d->data;
        int ad_len = d->length_data;
        int i = 0;
        /* Each AD structure is `field_len` bytes of payload preceded by the
         * length byte itself, so it ends at index i + field_len.  The bound was
         * `i + field_len >= ad_len`, which rejected the *last* structure whenever
         * it ended exactly at the end of the buffer — i.e. a correct, complete
         * packet was read as truncated.  A badge was only found because the
         * manufacturer data happens to be followed by something else in our own
         * advertising layout; move it to the end and every peer would vanish. */
        while (i + 1 < ad_len) {
            uint8_t field_len = ad[i];
            if (field_len == 0) break;

            int next = i + 1 + field_len;
            if (next > ad_len) break;   /* truncated structure */

            uint8_t field_type = ad[i + 1];

            if (field_type == 0xFF && field_len >= 19) {
                /* Manufacturer Specific Data */
                uint16_t company = ad[i + 2] | (ad[i + 3] << 8);
                if (company == BUDDY_MFG_COMPANY_ID) {
                    uint8_t ver = ad[i + 4];
                    if (ver == BUDDY_PROTO_VERSION) {
                        memcpy(dev_id, &ad[i + 5], 6);
                        memcpy(prof_hash, &ad[i + 11], 8);
                        mfg_flags = ad[i + 19];
                        is_buddy = true;
                    }
                }
            }
            i = next;
        }

        if (!is_buddy) break;

        /* Skip if privacy flag set */
        if (mfg_flags & 0x04) break;

        /* Peer tracking */
        peer_track_t *p = peer_find_or_add(d->addr.val);
        p->rssi = rssi;
        int64_t now = esp_timer_get_time() / 1000LL;

        char did[18];
        snprintf(did, sizeof(did), "%02x:%02x:%02x:%02x:%02x:%02x",
                 dev_id[0], dev_id[1], dev_id[2], dev_id[3], dev_id[4], dev_id[5]);
        strncpy(p->device_id, did, sizeof(p->device_id) - 1);

        buddy_proximity_feed(rssi);
        buddy_proximity_t prox = buddy_proximity_classify();
        if (prox != s_last_proximity) {
            ESP_LOGI(TAG, "Proximity: %s (rssi=%d)",
                     buddy_proximity_str(prox), rssi);
            s_last_proximity = prox;
        }

        /* Check if we should connect (before updating last_ad_ms, so
         * peer_connect_reject_reason() sees the PREVIOUS ad timestamp for dedup) */
        /* These five gates used to be silent `break`s, which cost real
         * debugging time: a rejected advertisement left no trace at all, so
         * "no connection happened" was indistinguishable from "the radio never
         * saw a peer".  Name the gate that rejected in one line.
         *
         * Note there is deliberately no check of our *own* privacy mode here.
         * BUDDY_FLAG_PRIVACY is something a badge broadcasts to ask peers not
         * to approach it, and that is honoured above, on the received flags.
         * Our own mode only decides whether *we* advertise; gating outbound
         * connections on it made a private badge unable to connect to anyone,
         * which silently killed the one-directional discovery test it was
         * being used for. */
        const char *reject = peer_connect_reject_reason(d->addr.val);
        if (!reject && !(mfg_flags & 0x01))      reject = "peer not accepting";
        else if (!reject && prox < BUDDY_PROX_NEAR) reject = "not near enough";
        else if (!reject && s_conn->active)      reject = "already connected";
        else if (!reject && s_conn->conn_handle != 0) reject = "incoming link";
        if (reject) {
            /* Logged at INFO for the cooldown gates: while the two badges sit
             * next to each other these fire once per advertisement, and they are
             * the difference between "we deliberately are not re-chatting" and
             * "the radio never saw the peer". */
            if (strcmp(reject, "peer cooldown") == 0) {
                ESP_LOGI(TAG, "Not connecting to %s: %s (%d ms since it was last "
                              "used)", did, reject,
                         (int)(now - p->last_conn_ms));
            } else if (strcmp(reject, "session cooldown") == 0) {
                ESP_LOGI(TAG, "Not connecting to %s: %s (%d ms since a conversation "
                              "ended)", did, reject,
                         (int)(now - s_last_session_end_ms));
            } else {
                ESP_LOGD(TAG, "Not connecting to %s: %s (rssi=%d flags=0x%02x prox=%s)",
                         did, reject, rssi, mfg_flags, buddy_proximity_str(prox));
            }
            break;
        }

        p->last_ad_ms = now;

        ESP_LOGI(TAG, "New buddy detected: %s (rssi=%d, prox=%s)",
                 did, rssi, buddy_proximity_str(prox));

        /* Initiate connection — cancel scan to free the radio */
        p->last_conn_ms = now;
        memset(s_conn, 0, sizeof(*s_conn));
        s_conn->active = true;
        s_conn->outgoing = true;
        s_conn->peer_flags = mfg_flags;
        memcpy(s_conn->peer_mac, d->addr.val, 6);
        strncpy(s_conn->peer_device_id, did, sizeof(s_conn->peer_device_id) - 1);
        s_conn->rssi = rssi;

        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(30));

        int rc = ble_gap_connect(s_own_addr_type, &d->addr,
                                 BLE_HS_FOREVER, NULL,
                                 buddy_ble_gap_event, NULL);
        if (rc != 0) {
            if (rc != BLE_HS_EDONE) {
                ESP_LOGW(TAG, "ble_gap_connect failed: %d", rc);
            }
            s_conn->active = false;
            s_conn->outgoing = false;
            /* Restart scanning */
            if (s_running) buddy_ble_start_scan();
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed: status=%d", event->connect.status);
            s_conn->active = false;
            s_conn->outgoing = false;
            if (s_running) buddy_ble_start_scan();
            break;
        }

        /* One link per encounter.  Both badges scan, so when two of them see
         * each other at close range at the same moment they each initiate, and
         * both links are accepted — the tracking state here is a single
         * connection, so the second one's bring-up silently overwrites the
         * first one's handles.  Each side then finds itself mid-subscribe on a
         * link whose peer is busy subscribing on the other one, and the CCCD
         * write tears down a link that was never fully configured (BLE reason
         * 531, CCCD_IMPROPERLY_CONFIGURED, is exactly that signature).
         *
         * Whichever role we are already committed to wins: if we are central,
         * keep that link and reject the incoming one, and vice versa.  Rejecting
         * here is cheap — the peer keeps scanning and the two meet again — while
         * letting both proceed corrupts the state above. */
        if (s_conn->active && s_conn->conn_handle != 0 &&
            event->connect.conn_handle != s_conn->conn_handle) {
            ESP_LOGW(TAG, "Second connection (handle=%d) while %s on handle=%d — "
                          "rejecting duplicate link",
                     event->connect.conn_handle,
                     s_conn->outgoing ? "central" : "peripheral",
                     s_conn->conn_handle);
            ble_gap_terminate(event->connect.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            break;
        }

        s_conn->conn_handle = event->connect.conn_handle;
        /* Set for incoming connections too, not just ones we started.  This is
         * the flag the advertisement handler uses to stay out of the way, and
         * the one DISC_COMPLETE uses to avoid restarting the scan underneath a
         * live connection.  Leaving it false for the peripheral role let that
         * device keep scanning at an 80% duty cycle for the whole connection. */
        s_conn->active = true;

        /* Fresh link — nothing about the previous chat path carries over. */
        s_att_mtu = BUDDY_ATT_MTU_DFLT;
        s_chat_link_up = false;
        s_chat_ready_sent = false;
        s_peer_subscribed = false;
        s_cccd_handle = 0;
        s_peer_chat_handle = 0;
        s_svc_start_handle = 0;
        s_svc_end_handle = 0;
        s_svc_found = false;

        /* And nothing about the previous peer: until this link's exchange
         * completes, the dialogue must not describe whoever was here last. */
        buddy_ble_clear_peer_profile();

        /* Fresh receive accumulator for the profile chunks, and no leftover
         * outgoing profile. */
        s_conn->profile_len = 0;
        s_conn->profile_sent = false;
        if (s_profile_tx) {
            heap_caps_free(s_profile_tx);
            s_profile_tx = NULL;
        }
        s_profile_tx_len = 0;
        s_profile_tx_off = 0;
        s_profile_rx_done = false;
        s_profile_read_offset = 0;

        if (s_conn->outgoing) {
            /* Chat wants a tight, low-latency link — but only one side may ask
             * for it.  Both badges run this firmware, so if both request the
             * update at the instant the link comes up, the two procedures
             * collide in the controller: the loser reports HCI 0x2A
             * (BLE_ERR_DIFF_TRANSACTION_COLLISION, logged as 554) and the
             * winner takes hundreds of milliseconds to land.  That storm sits
             * exactly on top of the ATT exchange the central is starting at the
             * same moment, and the subscriber end of a colliding procedure is
             * the one whose ATT traffic stalls — which is where the
             * characteristic discovery timeout comes from.  The central owns
             * the connection parameters; the peripheral leaves them alone. */
            chat_apply_conn_params(s_conn->conn_handle);

            /* The central drives MTU negotiation; a peripheral learns the
             * result from BLE_GAP_EVENT_MTU. Discovery chains off its callback. */
            ESP_LOGI(TAG, "Connected (handle=%d), exchanging MTU...",
                     s_conn->conn_handle);
            int rc = ble_gattc_exchange_mtu(s_conn->conn_handle, gatt_mtu_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "MTU exchange failed to start: %d", rc);
                gatt_start_discovery(s_conn->conn_handle);
            }
        } else {
            /* Incoming connection — peer will discover our services.
             *
             * Stop scanning first.  Scanning and the connection share one radio:
             * a peripheral that keeps a wide scan window open while it also has
             * to answer a 30 ms connection starves the connection, and the peer
             * sees that as ATT requests that are never answered.  The scan
             * currently runs a 300/1000 ms window (see buddy_ble.h); it resumes
             * on disconnect. */
            ble_gap_disc_cancel();
            /* Learn who called.  The scan path never ran for this link, so this
             * is the only chance to populate the peer identity. */
            buddy_ble_note_peer(s_conn->conn_handle);
            ESP_LOGI(TAG, "Incoming connection (handle=%d) from %s, scan stopped, waiting for peer to discover...",
                     s_conn->conn_handle, buddy_ble_peer_id());
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "Disconnect (reason=%d)", event->disconnect.reason);

        /* If we received a profile, post event */
        if (s_conn->profile_len > 0) {
            post_profile_event(s_conn->peer_mac, s_conn->peer_device_id,
                               s_conn->rssi, s_conn->profile_buf);
        }

        /* Tell the session before its view of the link is wiped. */
        if (s_chat_link_up) {
            chat_post(BUDDY_CHAT_RX_LINK_LOST, NULL, 0, s_conn->rssi);
        }

        memset(s_conn, 0, sizeof(*s_conn));
        s_att_mtu = BUDDY_ATT_MTU_DFLT;
        s_chat_link_up = false;
        s_chat_ready_sent = false;
        s_peer_subscribed = false;
        s_peer_chat_handle = 0;
        s_cccd_handle = 0;
        s_svc_start_handle = 0;
        s_svc_end_handle = 0;
        s_svc_found = false;

        /* Restart scanning for next buddy */
        if (s_running) buddy_ble_start_scan();
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        /* Scan stopped (either cycle end or cancelled for connection).
         * Only restart if we're not in the middle of a connection. */
        if (s_running && !s_conn->active) {
            buddy_ble_start_scan();
        }
        break;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* Peripheral -> central direction of the chat characteristic.  The
         * mbuf is only valid inside this callback, so copy before queueing. */
        if (event->notify_rx.attr_handle != s_peer_chat_handle) {
            ESP_LOGD(TAG, "Notification from unrelated handle 0x%04x",
                     event->notify_rx.attr_handle);
            break;
        }

        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len == 0 || len > BUDDY_CHAT_MSG_MAX) {
            ESP_LOGW(TAG, "Dropping chat notification of %u bytes", (unsigned)len);
            break;
        }

        uint8_t *buf = heap_caps_calloc(1, len, MALLOC_CAP_SPIRAM);
        if (!buf) break;
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        chat_post(BUDDY_CHAT_RX_DATA, buf, len, s_conn->rssi);
        heap_caps_free(buf);
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        /* Peripheral side: the peer just toggled notifications on our chat
         * characteristic.  That request is what tells us the chat path is up. */
        if (event->subscribe.attr_handle != s_local_chat_handle) break;

        s_peer_subscribed = event->subscribe.cur_notify != 0;
        ESP_LOGI(TAG, "Peer %s chat notifications",
                 s_peer_subscribed ? "enabled" : "disabled");

        if (s_peer_subscribed) {
            s_conn->peer_flags |= BUDDY_FLAG_CHAT_CAPABLE;
            chat_post_link_ready();
        }
        break;
    }

    case BLE_GAP_EVENT_MTU: {
        if (event->mtu.value > 0) {
            s_att_mtu = event->mtu.value;
            ESP_LOGI(TAG, "ATT MTU updated: %u (chat payload max %u)",
                     (unsigned)s_att_mtu, (unsigned)buddy_ble_chat_max_len());
        }
        break;
    }

    case BLE_GAP_EVENT_CONN_UPDATE: {
        if (event->conn_update.status != 0) {
            ESP_LOGW(TAG, "Connection param update failed: %d",
                     event->conn_update.status);
        } else {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "Connection params: itvl=%.2fms latency=%u timeout=%ums",
                         desc.conn_itvl * 1.25,
                         (unsigned)desc.conn_latency,
                         (unsigned)desc.supervision_timeout * 10);
            }
        }
        break;
    }

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        /* Falling through to the trailing `return 0` accepts the peer's
         * request, which is always what we want for a chat link. */
        ESP_LOGI(TAG, "Peer requested connection param change — accepting");
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "Advertising stopped unexpectedly (reason=%d)",
                 event->adv_complete.reason);
        break;
    }

    return 0;
}

/* ── GATT access callback (peripheral side) ────────────────────── */
static int buddy_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &g_buddy_chr_profile_uuid.u) == 0) {
        /* READ: hand back one window of our profile JSON.
         *
         * Not the whole value: at MTU 247 a single read response can carry about
         * 246 bytes, and the value is several hundred.  The offset of the window
         * was requested by the peer through a write to the write characteristic
         * (see the PROFILE_CMD_GET branch below), which is how this firmware
         * reads long values in both directions without relying on Read Blob or
         * Prepare/Execute — neither of which worked between two copies of it. */
        char *profile_json = heap_caps_calloc(1, BUDDY_PROFILE_MAX_BYTES, MALLOC_CAP_SPIRAM);
        if (!profile_json) return BLE_ATT_ERR_INSUFFICIENT_RES;

        int len = serialize_profile(profile_json, BUDDY_PROFILE_MAX_BYTES);
        if (len < 0) {
            heap_caps_free(profile_json);
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint32_t total = (uint32_t)len;
        uint32_t off = s_profile_read_offset;
        if (off >= total) {
            /* Nothing left (or nothing at all): a lone "last" marker. */
            ESP_LOGI(TAG, "Profile read: offset %u past end (%u) — sending end marker",
                     (unsigned)off, (unsigned)total);
            uint8_t end = CHAT_CHUNK_LAST;
            int end_rc = os_mbuf_append(ctxt->om, &end, 1);
            heap_caps_free(profile_json);
            return (end_rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        uint32_t take = total - off;
        if (take > PROFILE_READ_WINDOW) take = PROFILE_READ_WINDOW;
        uint8_t marker = (off + take < total) ? CHAT_CHUNK_MORE : CHAT_CHUNK_LAST;

        ESP_LOGI(TAG, "Profile read: window offset=%u total=%u take=%u marker='%c'",
                 (unsigned)off, (unsigned)total, (unsigned)take, (char)marker);

        int rc = os_mbuf_append(ctxt->om, &marker, 1);
        if (rc == 0) rc = os_mbuf_append(ctxt->om, profile_json + off, take);
        heap_caps_free(profile_json);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(uuid, &g_buddy_chr_profile_write_uuid.u) == 0) {
        /* WRITE: either a read request (a small command) or one chunk of the
         * peer's profile.  See the marker definitions at the top. */
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len == 0 || om_len > BUDDY_PROF_CHUNK_MAX) {
            ESP_LOGW(TAG, "Profile write of %u bytes rejected", (unsigned)om_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t marker = 0;
        os_mbuf_copydata(ctxt->om, 0, 1, &marker);

        if (marker == PROFILE_CMD_GET) {
            if (om_len != 3) {
                ESP_LOGW(TAG, "Profile read command of %u bytes (expected 3) rejected",
                         (unsigned)om_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            uint8_t off_bytes[2] = {0};
            os_mbuf_copydata(ctxt->om, 1, 2, off_bytes);
            s_profile_read_offset = (uint16_t)(off_bytes[0] | (off_bytes[1] << 8));
            ESP_LOGI(TAG, "Profile read command: next window at offset %u",
                     (unsigned)s_profile_read_offset);
            return 0;
        }

        /* Get peer MAC from connection info */
        uint8_t peer_mac[6] = {0};
        int8_t rssi = -90;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(conn_handle, &desc) == 0) {
            memcpy(peer_mac, desc.peer_id_addr.val, 6);
        }
        /* Also use s_conn if we initiated (has RSSI from scan) */
        if (memcmp(peer_mac, s_conn->peer_mac, 6) == 0 && s_conn->rssi != 0) {
            rssi = s_conn->rssi;
        }

        if (om_len == 1) {
            /* Marker-only chunk: the peer's profile is empty. */
            post_profile_event(peer_mac, "", rssi, NULL);
            return 0;
        }

        size_t space = sizeof(s_conn->profile_buf) - 1 - s_conn->profile_len;
        if ((size_t)(om_len - 1) > space) {
            ESP_LOGW(TAG, "Peer profile exceeds %u bytes, ignoring it",
                     (unsigned)(sizeof(s_conn->profile_buf) - 1));
            s_conn->profile_len = 0;
            return 0;
        }

        os_mbuf_copydata(ctxt->om, 1, om_len - 1,
                         s_conn->profile_buf + s_conn->profile_len);
        s_conn->profile_len += om_len - 1;
        s_conn->profile_buf[s_conn->profile_len] = '\0';

        if (marker != CHAT_CHUNK_LAST) {
            return 0;   /* more to come */
        }

        /* Extract device_id, then hand the whole thing on. */
        char did[18] = "unknown";
        cJSON *root = cJSON_Parse(s_conn->profile_buf);
        if (root) {
            cJSON *didj = cJSON_GetObjectItem(root, "did");
            if (didj && cJSON_IsString(didj)) {
                strncpy(did, didj->valuestring, sizeof(did) - 1);
            }
            cJSON_Delete(root);
        }

        post_profile_event(peer_mac, did, rssi, s_conn->profile_buf);
        s_conn->profile_len = 0;
        return 0;
    }

    if (ble_uuid_cmp(uuid, &g_buddy_chr_chat_uuid.u) == 0) {
        /* WRITE: central -> peripheral direction of the chat characteristic.
         * One ATT write carries exactly one frame, so no reassembly is needed
         * as long as the peer respects the negotiated MTU. */
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
        }

        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len == 0 || om_len > BUDDY_CHAT_MSG_MAX) {
            ESP_LOGW(TAG, "Chat write of %u bytes rejected", (unsigned)om_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t *buf = heap_caps_calloc(1, om_len, MALLOC_CAP_SPIRAM);
        if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
        os_mbuf_copydata(ctxt->om, 0, om_len, buf);
        chat_post(BUDDY_CHAT_RX_DATA, buf, om_len, s_conn->rssi);
        heap_caps_free(buf);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── GATT service definition ───────────────────────────────────── */
static struct ble_gatt_chr_def g_buddy_chrs[] = {
    {
        .uuid = &g_buddy_chr_profile_uuid.u,
        .access_cb = buddy_ble_gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &g_buddy_chr_profile_write_uuid.u,
        .access_cb = buddy_ble_gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE,
    },
    {
        /* Notify makes NimBLE register a CCCD for this characteristic, which
         * is what the central subscribes to.  WRITE_NO_RSP lets the central
         * use either write flavour. */
        .uuid = &g_buddy_chr_chat_uuid.u,
        .access_cb = buddy_ble_gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 }
};

static const struct ble_gatt_svc_def g_buddy_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_buddy_svc_uuid.u,
        .characteristics = g_buddy_chrs,
    },
    { 0 }
};

/* ── NimBLE sync / reset callbacks ─────────────────────────────── */
static void buddy_ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    /* The service definitions were queued in buddy_ble_init(); ble_gatts_start()
     * has already run once, from ble_hs_start(), and that call registered
     * GAP + GATT + ours.  Starting the ATT server again here would free and
     * rebuild the table, so this callback only reads back the result. */
    ble_att_set_preferred_mtu(BUDDY_CHAT_ATT_MTU);

    /* GAP's Device Name characteristic otherwise reports the sdkconfig default
     * ("nimble") to anyone who connects, which is a different name from the one
     * the same badge shows in a scan list. */
    rc = ble_svc_gap_device_name_set(buddy_ble_adv_name());
    if (rc != 0) {
        ESP_LOGW(TAG, "GAP device name set failed: %d", rc);
    }

    /* Resolve our own chat value handle — the peripheral notifies from it and
     * the subscribe event is matched against it. */
    rc = ble_gatts_find_chr(&g_buddy_svc_uuid.u, &g_buddy_chr_chat_uuid.u,
                            NULL, &s_local_chat_handle);
    if (rc != 0) {
        ESP_LOGE(TAG, "Chat characteristic lookup failed: %d", rc);
        s_local_chat_handle = 0;
    }

    /* Our own handle layout is the reference a peer's discovery log is read
     * against: a peer should report this service starting at this handle, and
     * should be able to answer read-by-type across it. */
    uint16_t svc_start = 0;
    if (ble_gatts_find_svc(&g_buddy_svc_uuid.u, &svc_start) != 0) {
        svc_start = 0;
    }

    ESP_LOGI(TAG, "NimBLE synced, svc=0x%04x chat=0x%04x (preferred MTU %u)",
             svc_start, s_local_chat_handle,
             (unsigned)ble_att_preferred_mtu());

    if (s_running) {
        buddy_ble_start_adv();
        buddy_ble_start_scan();
    }
}

static void buddy_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset (reason=%d)", reason);
}

/* ── GATT registration trace ───────────────────────────────────── */

/* Print the ATT handle map as the host assigns it.
 *
 * Handle numbers are otherwise invisible: the firmware can look up its own
 * chat handle, but not the layout a peer sees when it walks our table.  Every
 * discovery problem so far has had to be reasoned about from the other end's
 * log, so make our own table readable once at boot.  Fires from inside
 * ble_gatts_start(), before the first connection. */
static void buddy_ble_gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char uuid_str[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_str);
        ESP_LOGI(TAG, "  svc  handle=0x%04x  %s", ctxt->svc.handle, uuid_str);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_str);
        ESP_LOGI(TAG, "  chr  def=0x%04x val=0x%04x  %s",
                 ctxt->chr.def_handle, ctxt->chr.val_handle, uuid_str);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, uuid_str);
        ESP_LOGI(TAG, "  dsc  handle=0x%04x  %s", ctxt->dsc.handle, uuid_str);
        break;

    default:
        break;
    }
}

/* ── NimBLE host task ──────────────────────────────────────────── */
static void buddy_ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started — ATT handle map follows");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Public API ────────────────────────────────────────────────── */
esp_err_t buddy_ble_init(void)
{
    int rc;

    s_peers = heap_caps_calloc(PEER_TRACK_MAX, sizeof(*s_peers), MALLOC_CAP_SPIRAM);
    s_conn  = heap_caps_calloc(1, sizeof(*s_conn), MALLOC_CAP_SPIRAM);
    if (!s_peers || !s_conn) {
        ESP_LOGE(TAG, "Failed to allocate peers/conn in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    s_peer_count = 0;

    s_event_queue = xQueueCreate(4, sizeof(buddy_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* Chat has its own queue: the contact task blocks on an LLM call for tens
     * of seconds, and a chat frame must never wait behind that. */
    s_chat_queue = xQueueCreate(8, sizeof(buddy_chat_rx_t));
    if (!s_chat_queue) {
        ESP_LOGE(TAG, "Failed to create chat queue");
        return ESP_ERR_NO_MEM;
    }

    /* Guards the peer-profile copy shared between the host task and the chat. */
    s_peer_profile_lock = xSemaphoreCreateMutex();
    if (!s_peer_profile_lock) {
        ESP_LOGE(TAG, "Failed to create peer profile lock");
        return ESP_ERR_NO_MEM;
    }

    /* nimble_port_init() handles BT controller init + enable + NimBLE host init internally.
     * We must NOT call esp_bt_controller_init() ourselves — that would conflict. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = buddy_ble_on_reset;
    ble_hs_cfg.sync_cb = buddy_ble_on_sync;
    /* Nothing else in this firmware sets this; overwriting a host-internal
     * hook would be a real risk, so it is worth stating that the field is
     * unused here — ESP-IDF's own init path never touches it. */
    ble_hs_cfg.gatts_register_cb = buddy_ble_gatt_register_cb;

    /* Queue every service definition up before the host task starts.
     *
     * ble_hs_start() — which the host task runs — calls ble_gatts_start()
     * exactly once, and that one call is what turns the accumulated service
     * definitions into ATT server handles.  Do the registration from on_sync()
     * instead and the order inverts: ble_gatts_start() has already returned by
     * the time on_sync runs, so the only way to register is to start the ATT
     * server a second time.  ble_gatts_start() begins with ble_gatts_free_mem()
     * and ble_att_svr_start(), so that second call throws away the table the
     * first one built.
     *
     * That is why our service currently lands at handle 0x0001: nothing is
     * registered ahead of it.  GAP (0x1800) and GATT (0x1801) are missing from
     * the ATT table entirely — nothing in ESP-IDF's init path calls
     * ble_svc_gap_init() or ble_svc_gatt_init(), the application must, and we
     * never did.  A peer therefore sees a database with no GAP and no GATT
     * service and no Service Changed characteristic. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int num_handles = ble_gatts_count_cfg(g_buddy_svcs);
    if (num_handles < 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", num_handles);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(g_buddy_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(buddy_ble_host_task);

    /* Wait for NimBLE sync to complete */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "BLE transport initialized");
    return ESP_OK;
}

esp_err_t buddy_ble_start(void)
{
    if (s_running) return ESP_OK;

    s_running = true;

    /* If already synced, start now; otherwise on_sync will start us */
    if (ble_hs_synced()) {
        buddy_ble_start_adv();
        buddy_ble_start_scan();
    } else {
        ESP_LOGI(TAG, "Waiting for NimBLE sync...");
    }

    ESP_LOGI(TAG, "BLE transport started");
    return ESP_OK;
}

esp_err_t buddy_ble_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;

    ble_gap_adv_stop();
    /* Scan will stop when current cycle ends, or we could cancel */
    ble_gap_disc_cancel();

    if (s_conn->active) {
        ble_gap_terminate(s_conn->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    ESP_LOGI(TAG, "BLE transport stopped");
    return ESP_OK;
}

QueueHandle_t buddy_ble_get_event_queue(void)
{
    return s_event_queue;
}

/* ── Public chat transport API ─────────────────────────────────── */
QueueHandle_t buddy_ble_get_chat_queue(void)
{
    return s_chat_queue;
}

uint16_t buddy_ble_chat_max_len(void)
{
    uint16_t mtu = s_att_mtu > BUDDY_ATT_HEADER_LEN ? s_att_mtu : BUDDY_ATT_MTU_DFLT;
    uint16_t max = mtu - BUDDY_ATT_HEADER_LEN;
    return max > BUDDY_CHAT_MSG_MAX ? BUDDY_CHAT_MSG_MAX : max;
}

bool buddy_ble_is_central(void)
{
    return s_conn && s_conn->active && s_conn->outgoing;
}

const char *buddy_ble_peer_id(void)
{
    return (s_conn && s_conn->peer_device_id[0]) ? s_conn->peer_device_id : "";
}

int8_t buddy_ble_link_rssi(void)
{
    return (s_conn && s_conn->active) ? s_conn->rssi : 0;
}

void buddy_ble_terminate_link(void)
{
    if (s_conn && s_conn->active && s_conn->conn_handle != 0) {
        ble_gap_terminate(s_conn->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

esp_err_t buddy_ble_chat_send(const char *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_conn || !s_conn->active || s_conn->conn_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > buddy_ble_chat_max_len()) {
        ESP_LOGW(TAG, "Chat frame of %u bytes exceeds link max %u",
                 (unsigned)len, (unsigned)buddy_ble_chat_max_len());
        return ESP_ERR_INVALID_ARG;
    }

    if (s_conn->outgoing) {
        /* Central: write to the peer's characteristic.  A write request (not
         * no-response) is used deliberately — the response confirms delivery,
         * which the strict alternating turn order depends on. */
        if (!s_peer_chat_handle) return ESP_ERR_INVALID_STATE;

        int rc = ble_gattc_write_flat(s_conn->conn_handle, s_peer_chat_handle,
                                      data, (uint16_t)len,
                                      gatt_chat_write_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Chat write failed to start: %d", rc);
            return ESP_FAIL;
        }
    } else {
        /* Peripheral: notify back along the same characteristic. */
        if (!s_peer_subscribed || !s_local_chat_handle) {
            return ESP_ERR_INVALID_STATE;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
        if (!om) return ESP_ERR_NO_MEM;

        /* ble_gatts_notify_custom consumes the mbuf whatever the outcome, so
         * it must not be freed here even on failure. */
        int rc = ble_gatts_notify_custom(s_conn->conn_handle, s_local_chat_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "Chat notify failed: %d", rc);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}
