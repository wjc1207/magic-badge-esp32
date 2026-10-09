#include "buddy_profile.h"
#include "buddy.h"
#include "mimi_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

static const char *TAG = "buddy_profile";

static buddy_identity_t *s_identity = NULL;
static buddy_privacy_mode_t s_privacy = BUDDY_MODE_PUBLIC;

/* Cool-downs, cached in RAM.
 *
 * Cached rather than read on demand because the hot reader is
 * peer_connect_reject_reason(), which runs from the NimBLE GAP callback for
 * every advertisement in range — an NVS read there would put a flash access in
 * the radio's path.  Same shape as s_privacy above. */
static int64_t s_cooldown_peer_ms    = BUDDY_COOLDOWN_PEER_DEFAULT_MS;
static int64_t s_cooldown_session_ms = BUDDY_COOLDOWN_SESSION_DEFAULT_MS;

#define BUDDY_NVS_NS      "buddy"
#define BUDDY_NVS_KEY_ID  "identity"
#define BUDDY_NVS_KEY_PRIV "privacy"
/* Cool-downs, in milliseconds.  NVS keys cap at 15 characters. */
#define BUDDY_NVS_KEY_COOL_PEER "cool_peer"
#define BUDDY_NVS_KEY_COOL_SESS "cool_sess"
/* Was the single struct blob; erased on first boot under the per-field scheme.
 * Kept as a name so the cleanup can find it. */
#define BUDDY_NVS_KEY_LEGACY_PROF "profile"

/* ── Generate Ed25519 keypair ─────────────────────────────────── */
static esp_err_t generate_ed25519_keypair(uint8_t *pub, uint8_t *priv)
{
    int ret;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ecdsa_context ecdsa;
    const char *pers = "buddy_ed25519_gen";

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ecdsa_init(&ecdsa);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const uint8_t *)pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed: %d", ret);
        goto fail;
    }

    /* mbedTLS doesn't have Ed25519 in standard config — use ECDSA P-256 as signing identity
     * and keep X25519 for handshake key exchange (separate ephemeral keys).
     * Store the 32-byte ECDSA public key fingerprint as the device identity */
    ret = mbedtls_ecdsa_genkey(&ecdsa, MBEDTLS_ECP_DP_SECP256R1,
                               mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "ecdsa_genkey failed: %d", ret);
        goto fail;
    }

    /* Extract public key bytes */
    size_t olen = 0;
    ret = mbedtls_ecp_point_write_binary(&ecdsa.MBEDTLS_PRIVATE(grp), &ecdsa.MBEDTLS_PRIVATE(Q),
                                         MBEDTLS_ECP_PF_UNCOMPRESSED,
                                         &olen, pub, 65);
    /* For P-256: pub is 65 bytes uncompressed. Compress to 32-byte X coord. */
    if (olen == 65) {
        memmove(pub, pub + 1, 32);  /* skip 0x04 prefix, take X only */
    }

    /* Export private key (32 bytes for P-256) */
    ret = mbedtls_mpi_write_binary(&ecdsa.MBEDTLS_PRIVATE(d), priv, 32);
    if (ret != 0) {
        ESP_LOGE(TAG, "mpi_write_binary failed: %d", ret);
        goto fail;
    }

    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ESP_OK;

fail:
    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ESP_FAIL;
}

/* ── NVS helpers for the profile ──────────────────────────────────
 * One entry per field, so nothing here is tied to a struct layout and
 * BUDDY_PROF_KEY_* is the whole on-disk contract. */

/* Store a string field.  A NULL or empty value erases the key rather than
 * storing an empty string, which keeps "not set" and "cleared" the same state
 * and avoids growing the namespace with blank entries. */
static esp_err_t profile_put_str(nvs_handle_t nvs, const char *key, const char *value)
{
    if (!value || !value[0]) {
        esp_err_t err = nvs_erase_key(nvs, key);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    return nvs_set_str(nvs, key, value);
}

/* Read a field, mapping "absent" onto "empty". */
static esp_err_t profile_get_str(nvs_handle_t nvs, const char *key,
                                 char *out, size_t size)
{
    if (!out || size == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    size_t len = size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* Nothing stored, or a value that no longer fits this build's limit. */
        out[0] = '\0';
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return err;
}

/* ── Compute beacon profile hash ──────────────────────────────── */
/* The hash is what peers use to notice "this badge is not the one I met", so it
 * has to cover the fields the owner can edit — including the character fields,
 * otherwise rewriting how this person looks and behaves would leave the beacon
 * advertising the old identity. */
void buddy_profile_compute_hash(nvs_handle_t nvs, uint8_t hash_out[8])
{
    char name[BUDDY_DISPLAY_NAME_LEN]  = {0};
    char appearance[BUDDY_APPEARANCE_LEN] = {0};
    char belongings[BUDDY_BELONGINGS_LEN] = {0};
    char traits[BUDDY_TRAITS_LEN]      = {0};
    char tech[BUDDY_TECH_LEVEL_LEN]    = {0};

    profile_get_str(nvs, BUDDY_PROF_KEY_NAME, name, sizeof(name));
    profile_get_str(nvs, BUDDY_PROF_KEY_APPEARANCE, appearance, sizeof(appearance));
    profile_get_str(nvs, BUDDY_PROF_KEY_BELONGINGS, belongings, sizeof(belongings));
    profile_get_str(nvs, BUDDY_PROF_KEY_TRAITS, traits, sizeof(traits));
    profile_get_str(nvs, BUDDY_PROF_KEY_TECH_LEVEL, tech, sizeof(tech));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dn", name);
    cJSON_AddStringToObject(root, "ap", appearance);
    cJSON_AddStringToObject(root, "bl", belongings);
    cJSON_AddStringToObject(root, "tr", traits);
    cJSON_AddStringToObject(root, "tl", tech);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) { memset(hash_out, 0, 8); return; }

    uint8_t sha[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)json_str, strlen(json_str));
    mbedtls_sha256_finish(&ctx, sha);
    mbedtls_sha256_free(&ctx);

    memcpy(hash_out, sha, 8);
    free(json_str);
}

/* ── Load/generate identity ───────────────────────────────────── */
static esp_err_t identity_load_or_generate(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(*s_identity);
    err = nvs_get_blob(nvs, BUDDY_NVS_KEY_ID, s_identity, &len);
    if (err == ESP_OK && len == sizeof(*s_identity)) {
        ESP_LOGI(TAG, "Loaded device identity: %s", s_identity->device_id);
        nvs_close(nvs);
        return ESP_OK;
    }

    /* Generate new identity */
    ESP_LOGI(TAG, "Generating new device identity...");

    /* Use MAC as device_id */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_identity->device_id, sizeof(s_identity->device_id),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    err = generate_ed25519_keypair(s_identity->ed25519_public, s_identity->ed25519_private);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate keypair");
        nvs_close(nvs);
        return err;
    }

    err = nvs_set_blob(nvs, BUDDY_NVS_KEY_ID, s_identity, sizeof(*s_identity));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New device identity: %s", s_identity->device_id);
    }
    return err;
}

/* ── Load profile from NVS ────────────────────────────────────── */
/* Nothing is loaded into memory here: every field lives in its own NVS entry
 * and is read on demand.  This only seeds the defaults a brand-new device
 * should start with, and cleans up the old blob-based key. */
static esp_err_t profile_load(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    /* Privacy */
    uint8_t priv = BUDDY_MODE_PUBLIC;
    size_t plen = sizeof(priv);
    nvs_get_blob(nvs, BUDDY_NVS_KEY_PRIV, &priv, &plen);
    s_privacy = (buddy_privacy_mode_t)priv;

    /* Cool-downs.  Absent on a device that has never had them set, in which case
     * the defaults above stand.  Read as int64; anything outside the guard rails
     * (a stale value from an older build, a hand-edited NVS) falls back rather
     * than being honoured, so a bad stored number cannot lock the radio out. */
    int64_t cool = 0;
    if (nvs_get_i64(nvs, BUDDY_NVS_KEY_COOL_PEER, &cool) == ESP_OK &&
        cool >= BUDDY_COOLDOWN_MIN_MS && cool <= BUDDY_COOLDOWN_MAX_MS) {
        s_cooldown_peer_ms = cool;
    }
    if (nvs_get_i64(nvs, BUDDY_NVS_KEY_COOL_SESS, &cool) == ESP_OK &&
        cool >= BUDDY_COOLDOWN_MIN_MS && cool <= BUDDY_COOLDOWN_MAX_MS) {
        s_cooldown_session_ms = cool;
    }
    ESP_LOGI(TAG, "Cool-downs: peer %lld s, session %lld s",
             (long long)(s_cooldown_peer_ms / 1000),
             (long long)(s_cooldown_session_ms / 1000));

    char name[BUDDY_DISPLAY_NAME_LEN] = {0};
    err = profile_get_str(nvs, BUDDY_PROF_KEY_NAME, name, sizeof(name));

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot on this scheme.  If the old single-blob profile is still
         * around it is dropped — it cannot be reinterpreted safely — and the
         * owner gets the defaults plus a line in the log.
         *
         * A 4-byte probe distinguishes "absent" (NOT_FOUND) from "present but
         * larger than the probe" (INVALID_LENGTH). */
        uint8_t probe[4];
        size_t probe_len = sizeof(probe);
        esp_err_t probe_err = nvs_get_blob(nvs, BUDDY_NVS_KEY_LEGACY_PROF,
                                            probe, &probe_len);
        if (probe_err == ESP_OK || probe_err == ESP_ERR_NVS_INVALID_LENGTH) {
            ESP_LOGW(TAG, "Discarding the old blob profile; starting from defaults. "
                          "Fill the Character section once and it will stick from now on.");
            nvs_erase_key(nvs, BUDDY_NVS_KEY_LEGACY_PROF);
        }

        profile_put_str(nvs, BUDDY_PROF_KEY_NAME, "Buddy");
        profile_put_str(nvs, BUDDY_PROF_KEY_BIO, "Exploring the world with Buddy.");
    }

    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Profile storage ready (per-field NVS keys)");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════ */

esp_err_t buddy_profile_init(void)
{
    s_identity = heap_caps_calloc(1, sizeof(*s_identity), MALLOC_CAP_SPIRAM);
    if (!s_identity) {
        ESP_LOGE(TAG, "Failed to allocate identity in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = identity_load_or_generate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Identity init failed");
        return err;
    }

    return profile_load();
}

/* Read every field.  Used where the profile is rendered or sent as a whole; the
 * fields are stored separately, so this is a convenience view, not the format. */
esp_err_t buddy_profile_get(buddy_profile_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    profile_get_str(nvs, BUDDY_PROF_KEY_NAME, out->display_name, sizeof(out->display_name));
    profile_get_str(nvs, BUDDY_PROF_KEY_BIO, out->bio, sizeof(out->bio));
    profile_get_str(nvs, BUDDY_PROF_KEY_APPEARANCE, out->appearance, sizeof(out->appearance));
    profile_get_str(nvs, BUDDY_PROF_KEY_BELONGINGS, out->belongings, sizeof(out->belongings));
    profile_get_str(nvs, BUDDY_PROF_KEY_TRAITS, out->traits, sizeof(out->traits));
    profile_get_str(nvs, BUDDY_PROF_KEY_TECH_LEVEL, out->tech_level, sizeof(out->tech_level));
    profile_get_str(nvs, BUDDY_PROF_KEY_SPEECH, out->speech, sizeof(out->speech));
    profile_get_str(nvs, BUDDY_PROF_KEY_KNOWS, out->knows, sizeof(out->knows));
    profile_get_str(nvs, BUDDY_PROF_KEY_SCENE, out->scene, sizeof(out->scene));
    profile_get_str(nvs, BUDDY_PROF_KEY_SCENE_NAME, out->scene_name,
                    sizeof(out->scene_name));

    nvs_close(nvs);
    return ESP_OK;
}

/* Write every field.  The whole set is written at once because that is how the
 * config page submits it; each one lands in its own key. */
esp_err_t buddy_profile_set(const buddy_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = profile_put_str(nvs, BUDDY_PROF_KEY_NAME, profile->display_name);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_BIO, profile->bio);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_APPEARANCE, profile->appearance);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_BELONGINGS, profile->belongings);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_TRAITS, profile->traits);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_TECH_LEVEL, profile->tech_level);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_SPEECH, profile->speech);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_KNOWS, profile->knows);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_SCENE, profile->scene);
    if (err == ESP_OK) err = profile_put_str(nvs, BUDDY_PROF_KEY_SCENE_NAME,
                                             profile->scene_name);

    if (err == ESP_OK) {
        /* The beacon advertises this hash so a peer can tell this badge is not
         * the one it met before, so it is recomputed from the keys just written
         * rather than from the caller's copy. */
        uint8_t hash[8];
        buddy_profile_compute_hash(nvs, hash);
        err = nvs_set_blob(nvs, BUDDY_PROF_KEY_HASH, hash, sizeof(hash));
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Profile saved: name=%s", profile->display_name);
    return err;
}

const buddy_identity_t *buddy_identity_get(void)
{
    return s_identity;
}

esp_err_t buddy_privacy_set(buddy_privacy_mode_t mode)
{
    s_privacy = mode;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        uint8_t v = (uint8_t)mode;
        nvs_set_blob(nvs, BUDDY_NVS_KEY_PRIV, &v, sizeof(v));
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Privacy mode: %s", mode == BUDDY_MODE_PUBLIC ? "PUBLIC" : "PRIVATE");
    return ESP_OK;
}

buddy_privacy_mode_t buddy_privacy_get(void)
{
    return s_privacy;
}

/* ── Re-chat cool-downs ───────────────────────────────────────── */
static int64_t clamp_cooldown(int64_t ms)
{
    if (ms < BUDDY_COOLDOWN_MIN_MS) return BUDDY_COOLDOWN_MIN_MS;
    if (ms > BUDDY_COOLDOWN_MAX_MS) return BUDDY_COOLDOWN_MAX_MS;
    return ms;
}

esp_err_t buddy_cooldowns_set(int64_t peer_ms, int64_t session_ms)
{
    s_cooldown_peer_ms    = clamp_cooldown(peer_ms);
    s_cooldown_session_ms = clamp_cooldown(session_ms);

    /* Applied to the in-memory values above before the write, so the gates
     * change even if NVS is full or read-only: the setting is what the radio
     * uses, and losing it across a reboot is a smaller failure than having the
     * page report a value the firmware is not using. */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_i64(nvs, BUDDY_NVS_KEY_COOL_PEER, s_cooldown_peer_ms);
        if (err == ESP_OK) {
            err = nvs_set_i64(nvs, BUDDY_NVS_KEY_COOL_SESS, s_cooldown_session_ms);
        }
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Cool-downs set: peer %lld s, session %lld s",
             (long long)(s_cooldown_peer_ms / 1000),
             (long long)(s_cooldown_session_ms / 1000));
    return err;
}

int64_t buddy_cooldown_peer_ms(void)    { return s_cooldown_peer_ms; }
int64_t buddy_cooldown_session_ms(void) { return s_cooldown_session_ms; }

bool buddy_profile_get_hash(uint8_t hash_out[8])
{
    if (!hash_out) return false;
    memset(hash_out, 0, 8);

    nvs_handle_t nvs;
    if (nvs_open(BUDDY_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t len = 8;
    bool ok = (nvs_get_blob(nvs, BUDDY_PROF_KEY_HASH, hash_out, &len) == ESP_OK &&
               len == 8);
    nvs_close(nvs);
    return ok;
}

esp_err_t buddy_profile_get_field(const char *key, char *out, size_t size)
{
    if (!key || !out || size == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    err = profile_get_str(nvs, key, out, size);
    nvs_close(nvs);
    return err;
}

/* Read several fields over one NVS open.  The pair accessor below is the only
 * caller that wants more than one field at a time, and opening the namespace
 * six times for one turn is wasteful on a device this tight on memory. */
size_t buddy_profile_get_fields(const char **keys, char *const *outs,
                                const size_t *sizes, size_t count)
{
    if (!keys || !outs || !sizes || count == 0) return 0;

    nvs_handle_t nvs;
    if (nvs_open(BUDDY_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return 0;

    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        if (profile_get_str(nvs, keys[i], outs[i], sizes[i]) == ESP_OK) found++;
    }

    nvs_close(nvs);
    return found;
}

esp_err_t buddy_profile_set_field(const char *key, const char *value)
{
    if (!key) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = profile_put_str(nvs, key, value);
    if (err == ESP_OK) {
        /* The beacon's hash covers the character fields, so it has to be
         * refreshed after any single-field edit too — otherwise the owner could
         * rewrite how this person looks and peers would still recognise the old
         * identity from the beacon. */
        uint8_t hash[8];
        buddy_profile_compute_hash(nvs, hash);
        err = nvs_set_blob(nvs, BUDDY_PROF_KEY_HASH, hash, sizeof(hash));
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
