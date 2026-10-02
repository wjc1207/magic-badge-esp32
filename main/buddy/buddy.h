#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* BLE timing lives in buddy_ble.h, next to the rest of the transport
 * configuration — it used to be duplicated here, which meant a change to one
 * copy silently lost to whichever header was included last. */

/* ── Beacon timing ───────────────────────────────────────────── */
#define BUDDY_BEACON_PERIOD_MS      250
#define BUDDY_BEACON_JITTER_MS      50
#define BUDDY_PROFILE_HASH_LEN      8

/* ── Proximity thresholds (RSSI dBm) ──────────────────────────── */
#define BUDDY_PROXIMITY_NEAR        (-55)
#define BUDDY_PROXIMITY_MID         (-70)
#define BUDDY_PROXIMITY_FAR         (-85)
#define BUDDY_RSSI_SAMPLES          5

/* ── Discovery limits ────────────────────────────────────────── */
#define BUDDY_BEACON_DEDUP_MS       2000
#define BUDDY_REHANDSHAKE_COOLDOWN_S (30 * 60)

/* ── Profile limits ──────────────────────────────────────────── */
/* Upper bound on a profile *exchanged over BLE* — the buffer that holds the
 * serialized JSON, and the largest blob a peer is allowed to write.  The
 * character fields below are local-only (never serialized, see
 * serialize_profile()) so they do not have to fit here, but the struct they
 * belong to does: it is stored as one NVS blob and copied around as a unit.
 * The _Static_assert below keeps the two in step. */
#define BUDDY_PROFILE_MAX_BYTES     4096
#define BUDDY_DEVICE_ID_LEN         18
#define BUDDY_DISPLAY_NAME_LEN      32
#define BUDDY_BIO_LEN               1024
/* Character material is only ever read by this badge's own model, so these are
 * sized for comfortable prose rather than for a radio frame. */
#define BUDDY_APPEARANCE_LEN        512   /* what the person looks like */
#define BUDDY_BELONGINGS_LEN        512   /* what they carry on them */
#define BUDDY_TRAITS_LEN            128   /* three words, comma separated */
#define BUDDY_TECH_LEVEL_LEN        64
#define BUDDY_MAX_CONTACTS          500

/* ── Proximity classes ───────────────────────────────────────── */
typedef enum {
    BUDDY_PROX_UNKNOWN = 0,
    BUDDY_PROX_FAR,
    BUDDY_PROX_MID,
    BUDDY_PROX_NEAR,
} buddy_proximity_t;

/* ── Contact status ──────────────────────────────────────────── */
typedef enum {
    BUDDY_CONTACT_NEW = 0,
    BUDDY_CONTACT_KNOWN,
    BUDDY_CONTACT_RECENT,
} buddy_contact_status_t;

/* ── Privacy mode ────────────────────────────────────────────── */
typedef enum {
    BUDDY_MODE_PUBLIC = 0,
    BUDDY_MODE_PRIVATE,
} buddy_privacy_mode_t;

/* ── User profile ────────────────────────────────────────────── */
/* Bump BUDDY_PROFILE_VERSION whenever a field is added or removed.
 *
 * This struct is what goes into the NVS blob, so its layout is on-disk format:
 * an older blob is shorter, and nvs_get_blob() only reports that it was shorter
 * — the bytes past its end stay zero in the new struct, which reads as "the
 * user cleared every field that came after".  profile_load() rejects a version
 * it does not know rather than silently presenting an empty profile. */
#define BUDDY_PROFILE_VERSION   4

typedef struct {
    uint8_t  version;
    char     display_name[BUDDY_DISPLAY_NAME_LEN];
    char     bio[BUDDY_BIO_LEN];
    /* Character material for the live chat: what this person looks like, what
     * they carry, three words for their temperament, and how technical they
     * are.  These are what turn a name into somebody the model can speak as.
     *
     * Local only — never serialized onto the BLE profile characteristic.  Each
     * badge speaks as its own wearer from its own copy, so the peer's model has
     * no use for them, and that characteristic is readable by any device that
     * connects (no pairing, no authentication). */
    char     appearance[BUDDY_APPEARANCE_LEN];
    char     belongings[BUDDY_BELONGINGS_LEN];
    char     traits[BUDDY_TRAITS_LEN];        /* three words, comma separated */
    char     tech_level[BUDDY_TECH_LEVEL_LEN];
    uint8_t  profile_hash[BUDDY_PROFILE_HASH_LEN];
} buddy_profile_t;

/* Catch, at build time, the two ways this layout can break:
 *
 *  - the struct outgrowing BUDDY_PROFILE_MAX_BYTES, which is the size NVS writes
 *    are validated against — get that wrong and a "saved" profile is judged
 *    invalid on the next boot and silently replaced with defaults;
 *  - the struct outgrowing BUDDY_PROFILE_MAX_BYTES - 64, which the GATT server
 *    refuses to write, so a peer could never send a full one.
 *
 * A failing assertion here means adjusting a length constant, not debugging
 * Bluetooth. */
_Static_assert(sizeof(buddy_profile_t) <= BUDDY_PROFILE_MAX_BYTES,
               "buddy_profile_t does not fit BUDDY_PROFILE_MAX_BYTES");
_Static_assert(sizeof(buddy_profile_t) + 64 <= BUDDY_PROFILE_MAX_BYTES,
               "buddy_profile_t leaves no room for a peer's profile write");

/* ── Device identity (generated once at first boot) ──────────── */
typedef struct {
    char     device_id[BUDDY_DEVICE_ID_LEN];   /* MAC string */
    uint8_t  ed25519_public[32];
    uint8_t  ed25519_private[32];              /* stored in NVS, never transmitted */
} buddy_identity_t;

/* ── Contact record (stored on-device only) ──────────────────── */
/* What is known about a badge this one has met.  Kept deliberately small: the
 * match score, icebreaker and shared-interests fields went away with the
 * tag-based matching feature they belonged to. */
typedef struct {
    char     peer_id[BUDDY_DEVICE_ID_LEN];
    char     display_name[BUDDY_DISPLAY_NAME_LEN];
    char     bio[BUDDY_BIO_LEN];
    int64_t  last_met_unix;
    uint16_t meeting_count;
} buddy_contact_record_t;

/* ── Internal buddy event ────────────────────────────────────── */
typedef enum {
    BUDDY_EVT_PEER_DISCOVERED = 0,
    BUDDY_EVT_HANDSHAKE_COMPLETE,
    BUDDY_EVT_HANDSHAKE_FAILED,
    BUDDY_EVT_PROFILE_READY,
} buddy_event_type_t;

typedef struct {
    buddy_event_type_t type;
    uint8_t  peer_mac[6];
    char     peer_device_id[BUDDY_DEVICE_ID_LEN];
    int8_t   rssi;
    buddy_proximity_t proximity;
    buddy_profile_t   *peer_profile;   /* allocated from PSRAM, consumer frees */
    bool     peer_profile_valid;
} buddy_event_t;

/* ── LED feedback ────────────────────────────────────────────── */
typedef enum {
    BUDDY_LED_PATTERN_OFF = 0,
    BUDDY_LED_PATTERN_BLUE_SLOW,
    BUDDY_LED_PATTERN_AMBER,
    BUDDY_LED_PATTERN_GREEN_FAST,
    BUDDY_LED_PATTERN_AMBER_BRIEF,    /* missed - no WiFi */
    BUDDY_LED_PATTERN_CHAT,           /* live conversation with a peer */
} buddy_led_pattern_t;

/* ══════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════ */

/**
 * Initialize the full buddy subsystem:
 *   - Load/generate device identity (Ed25519 keypair)
 *   - Load user profile from NVS
 *   - Mount contact store from SPIFFS
 *   - Initialize BLE transport
 *   - Start beacon TX and RX tasks
 *   - Start contact processing task
 *   - Init LED
 */
esp_err_t buddy_init(void);

/**
 * Start buddy discovery (BLE advertising + scanning).
 */
esp_err_t buddy_start(void);

/**
 * Stop buddy discovery (BLE advertising + scanning + connections).
 */
esp_err_t buddy_stop(void);

/**
 * Get the event queue for contact processing.
 */
QueueHandle_t buddy_ble_get_event_queue(void);

/**
 * Get the current user profile. Caller provides buffer.
 */
esp_err_t buddy_profile_get(buddy_profile_t *out);

/**
 * Save user profile to NVS and recompute beacon hash.
 */
esp_err_t buddy_profile_set(const buddy_profile_t *profile);

/**
 * Get device identity.
 */
const buddy_identity_t *buddy_identity_get(void);

/**
 * Set privacy mode (PUBLIC = broadcasting, PRIVATE = silent).
 */
esp_err_t buddy_privacy_set(buddy_privacy_mode_t mode);

/**
 * Get current privacy mode.
 */
buddy_privacy_mode_t buddy_privacy_get(void);

/**
 * Look up a contact record by peer device_id.
 * Returns ESP_OK and fills *out, or ESP_ERR_NOT_FOUND.
 */
esp_err_t buddy_contacts_get(const char *peer_id, buddy_contact_record_t *out);

/**
 * List contacts. Fills buf with up to max records. *count is updated.
 */
esp_err_t buddy_contacts_list(buddy_contact_record_t *buf, size_t max, size_t *count);

/**
 * Set LED pattern. Non-blocking, async-safe.
 */
esp_err_t buddy_led_set(buddy_led_pattern_t pattern);

/**
 * cleanup all contacts and reset the contact store (for debug)
 * tool_exec write_file {"path":"/spiffs/contacts.json","content":"[]"}
 */