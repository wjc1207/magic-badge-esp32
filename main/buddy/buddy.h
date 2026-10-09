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
/* BUDDY_REHANDSHAKE_COOLDOWN_S (30 minutes) used to live here.  It was never
 * referenced by anything, and the value that actually governs how often two
 * badges may talk is buddy_cooldown_peer_ms() — three minutes by default, and
 * settable from the config page.  Deleted rather than left as a trap: reading it
 * implies a behaviour the firmware does not have. */

/* ── Profile limits ──────────────────────────────────────────── */
/* Upper bound on a profile *exchanged over BLE*: the buffer that holds the
 * serialized JSON, and the largest write a peer may send.  It is a wire limit,
 * not a storage or layout constraint — the profile itself is kept as one NVS
 * entry per field (see BUDDY_PROF_KEY_* below), so nothing here is on-disk
 * format.
 *
 * Roomier than the current JSON needs, on purpose: an older peer may still send
 * the tag/contact block that this firmware no longer stores, and rejecting its
 * write would silently break the exchange. */
#define BUDDY_PROFILE_MAX_BYTES     4096
#define BUDDY_DEVICE_ID_LEN         18
#define BUDDY_DISPLAY_NAME_LEN      32
#define BUDDY_BIO_LEN               2048
/* Character material is only ever read by this badge's own model, so these are
 * sized for comfortable prose rather than for a radio frame.  NVS allows about
 * 4000 bytes per entry, which is the only real ceiling here. */
#define BUDDY_APPEARANCE_LEN        1024  /* what the person looks like */
#define BUDDY_BELONGINGS_LEN        1024  /* what they carry on them */
#define BUDDY_TRAITS_LEN            256   /* three words, comma separated */
#define BUDDY_TECH_LEVEL_LEN        128   /* the setting's technology, not the wearer's skill */
/* How this person talks: dialect, register, how long their sentences run.
 *
 * Separate from tech_level because they are different things and conflating them
 * broke both.  A character sheet that used tech_level to say "普通话为主，带轻粤语"
 * left the field holding something its own form never asked for: the input is
 * labelled "World tech level" and is meant for the setting the character lives
 * in.
 *
 * Roomier than tech_level on purpose: this is the single most load-bearing field
 * for making the model speak as a particular person, and it needs room for a
 * short prohibition list. */
#define BUDDY_SPEECH_LEN            256
/* The edge of this character's knowledge: the things they have heard of.
 *
 * Two characters can come from settings that do not share a single proper noun,
 * and a badge exchanges only appearance and belongings — no name, no history, no
 * world.  Without this, a crossover encounter is whatever the model happens to
 * infer from a stranger's coat.  With it, the model has an explicit boundary to
 * run the other person's belongings against, and "what is that?" becomes a
 * natural turn instead of luck.
 *
 * A list rather than prose: proper nouns and concepts, not a paragraph.  The
 * firmware supplies the sentence that turns it into a boundary, so every wearer
 * gets the same behaviour and nobody has to phrase it themselves. */
#define BUDDY_KNOWS_LEN             512
#define BUDDY_MAX_CONTACTS          500

/* The preset scene for this badge's encounters.
 *
 * A scene belongs to an ENCOUNTER, not to a character: it is shared ground the
 * two badges both stand in, which is why it is a separate card from the
 * character card and is not exchanged over BLE — each side keeps its own and
 * neither reads the other's.
 *
 * Preset, not generated. Generating a scene means a model call before the first
 * word is spoken, which doubles the latency of every meeting and produces a
 * different scene for each badge, so the two would no longer be standing in the
 * same place. The owner writes one offline (the config page offers keyword
 * generation as an external tool) and uploads it.
 *
 * Sized for the detailed end of what the sandbox experiments found useful: a
 * scene with twenty-odd nameable things runs to 300-400 characters, which is
 * ~1200 bytes in UTF-8. The sandbox measurement was blunt about why that matters
 * — a one-line scene gives the conversation about one line of material, and the
 * rest is carried by the cards. */
#define BUDDY_SCENE_LEN             1280
/* A label for the scene, used to name the exported file.
 *
 * Not sent to the other badge and not part of the prompt: it is what makes a
 * folder of exported scenes readable, and only the owner ever sees it. Sized
 * like the display name because it is the same kind of thing. */
#define BUDDY_SCENE_NAME_LEN        32

/* ── Profile NVS keys ────────────────────────────────────────── */
/* The user profile is stored as one NVS entry per field.  NVS is a key-value
 * store, and keeping the profile as a single opaque blob meant every layout
 * change invalidated it and silently reset the owner's settings on the next
 * boot.
 *
 * One key per field makes adding a field free: an existing device simply has no
 * value for the new key and the default applies.  No version number, nothing to
 * migrate, nothing to remember.
 *
 * NVS caps key names at 15 characters. */
#define BUDDY_PROF_KEY_NAME        "name"
#define BUDDY_PROF_KEY_BIO         "bio"
#define BUDDY_PROF_KEY_APPEARANCE  "appearance"
#define BUDDY_PROF_KEY_BELONGINGS  "belongings"
#define BUDDY_PROF_KEY_TRAITS      "traits"
#define BUDDY_PROF_KEY_TECH_LEVEL  "tech_level"
#define BUDDY_PROF_KEY_SPEECH      "speech"
#define BUDDY_PROF_KEY_KNOWS       "knows"
#define BUDDY_PROF_KEY_SCENE       "scene"
#define BUDDY_PROF_KEY_SCENE_NAME  "scene_name"
#define BUDDY_PROF_KEY_HASH        "prof_hash"

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
} buddy_contact_status_t;

/* ── Privacy mode ────────────────────────────────────────────── */
typedef enum {
    BUDDY_MODE_PUBLIC = 0,
    BUDDY_MODE_PRIVATE,
} buddy_privacy_mode_t;

/* ── User profile ────────────────────────────────────────────── */
/* The fields, together, for the places that render or receive the profile as a
 * whole: the config page, `buddy_status`, and the JSON form exchanged over BLE.
 *
 * This is a *view*, not storage.  Everything is written per field (see the
 * BUDDY_PROF_KEY_* names above), so no layout here is on-disk format and none
 * of it can invalidate what the owner already saved. */
typedef struct {
    char     display_name[BUDDY_DISPLAY_NAME_LEN];
    char     bio[BUDDY_BIO_LEN];
    /* Character material for the live chat: what this person looks like, what
     * they carry, three words for their temperament, how technical they are, and
     * how they talk.  These are what turn a name into somebody the model can
     * speak as.
     *
     * Local only — never serialized onto the BLE profile characteristic.  Each
     * badge speaks as its own wearer from its own copy, so the peer's model has
     * no use for them, and that characteristic is readable by any device that
     * connects (no pairing, no authentication). */
    char     appearance[BUDDY_APPEARANCE_LEN];
    char     belongings[BUDDY_BELONGINGS_LEN];
    char     traits[BUDDY_TRAITS_LEN];        /* three words, comma separated */
    char     tech_level[BUDDY_TECH_LEVEL_LEN];
    char     speech[BUDDY_SPEECH_LEN];        /* dialect and register */
    char     knows[BUDDY_KNOWS_LEN];          /* the edge of what this character knows */
    /* The preset scene, if the owner set one.  Also local only, and for a
     * stronger reason than the fields above: the two badges are meant to be
     * standing in the same place, so if either read the other's copy they could
     * disagree.  A stranger's badge must not be able to describe the room. */
    char     scene[BUDDY_SCENE_LEN];
    /* What the owner calls that scene, for the exported file's name. Deliberately
     * after `scene` and never transmitted: it labels the owner's own copy, and a
     * scene adopted from the other badge arrives without one — the room is not
     * theirs to name. */
    char     scene_name[BUDDY_SCENE_NAME_LEN];
} buddy_profile_t;

/* ── Device identity (generated once at first boot) ──────────── */
typedef struct {
    char     device_id[BUDDY_DEVICE_ID_LEN];   /* MAC string */
    uint8_t  ed25519_public[32];
    uint8_t  ed25519_private[32];              /* stored in NVS, never transmitted */
} buddy_identity_t;

/* ── Contact record (stored on-device only) ──────────────────── */
/* What is known about a badge this one has met: its device id and what the
 * profile exchange revealed about its wearer.
 *
 * No name and no bio — neither is transmitted, and a badge only learns a name if
 * the person says it during a conversation.  The match score, icebreaker and
 * shared-interests fields went away with the tag-based matching feature they
 * belonged to. */
typedef struct {
    char     peer_id[BUDDY_DEVICE_ID_LEN];
    char     appearance[BUDDY_APPEARANCE_LEN];
    char     belongings[BUDDY_BELONGINGS_LEN];
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

/* Carries what a link actually revealed: the peer's identity and the visible
 * half of its wearer.  Small by design — the consumer stores it, not a pointer
 * to something it has to remember to free. */
typedef struct {
    buddy_event_type_t type;
    uint8_t  peer_mac[6];
    char     peer_device_id[BUDDY_DEVICE_ID_LEN];
    int8_t   rssi;
    buddy_proximity_t proximity;
    char     peer_appearance[BUDDY_APPEARANCE_LEN];
    char     peer_belongings[BUDDY_BELONGINGS_LEN];
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

/* ── Re-chat cool-downs ───────────────────────────────────────── */
/* How long a badge waits before it will open another chat link.
 *
 * Two gates, both in milliseconds, both settable from the config page:
 *
 *   session — after *any* conversation ended, no new link to anyone.  This is
 *             the backstop: it keys on nothing but the clock, so it still works
 *             when peer identification gets the peer's address wrong (which is
 *             what once let two badges talk round after round forever).
 *   peer    — after talking to *this* peer, no new link to it.  This is the one
 *             that sets the real pace; the session gate only ever needs to cover
 *             the moment of disconnect.
 *
 * The right value depends entirely on the situation.  Side by side at an event,
 * three minutes is right: long enough that the two badges stop reconnecting,
 * short enough that they can talk again after walking apart and back.  For two
 * people who meet once a day, three minutes is irrelevant and hours would be
 * closer to the intent.  Hence the setting rather than a compile-time number. */
#define BUDDY_COOLDOWN_PEER_DEFAULT_MS    (3 * 60 * 1000LL)
#define BUDDY_COOLDOWN_SESSION_DEFAULT_MS (3 * 60 * 1000LL)
/* Guard rails for anything typed into the config page.  A zero peer cool-down
 * means two badges in range reconnect the instant a session ends, so it is not
 * offered; the cap keeps a mistyped number from outliving the event. */
#define BUDDY_COOLDOWN_MIN_MS             (10 * 1000LL)
#define BUDDY_COOLDOWN_MAX_MS             (7 * 24 * 60 * 60 * 1000LL)

/**
 * Set the cool-downs. Values are clamped to the guard rails above. Persisted,
 * and effective immediately (no restart needed for the BLE gates).
 */
esp_err_t buddy_cooldowns_set(int64_t peer_ms, int64_t session_ms);

/** Cool-down after talking to a specific peer, in milliseconds. */
int64_t buddy_cooldown_peer_ms(void);

/** Cool-down after any conversation, in milliseconds. */
int64_t buddy_cooldown_session_ms(void);

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