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
static buddy_profile_t   *s_profile = NULL;
static buddy_privacy_mode_t s_privacy = BUDDY_MODE_PUBLIC;

#define BUDDY_NVS_NS      "buddy"
#define BUDDY_NVS_KEY_ID  "identity"
#define BUDDY_NVS_KEY_PROF "profile"
#define BUDDY_NVS_KEY_PRIV "privacy"

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

/* ── Compute beacon profile hash ──────────────────────────────── */
/* The hash is what peers use to notice "this badge is not the one I met", so it
 * has to cover the fields the owner can edit — including the character fields,
 * otherwise rewriting how this person looks and behaves would leave the beacon
 * advertising the old identity. */
void buddy_profile_compute_hash(const buddy_profile_t *profile, uint8_t hash_out[8])
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dn", profile->display_name);
    cJSON_AddStringToObject(root, "ap", profile->appearance);
    cJSON_AddStringToObject(root, "bl", profile->belongings);
    cJSON_AddStringToObject(root, "tr", profile->traits);
    cJSON_AddStringToObject(root, "tl", profile->tech_level);

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
static esp_err_t profile_load(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t len = sizeof(*s_profile);
    err = nvs_get_blob(nvs, BUDDY_NVS_KEY_PROF, s_profile, &len);

    if (err == ESP_OK) {
        /* The blob is a raw struct, so it is only meaningful if it was written
         * by this exact layout.  An older blob is shorter, and the tail of the
         * new struct would stay zero — which reads as "the owner emptied every
         * field that did not exist when they saved", and would advertise an
         * identity they never chose.  Reject it instead and let the caller fall
         * back to defaults; the owner re-saves once from the config page. */
        if (len != sizeof(*s_profile) || s_profile->version != BUDDY_PROFILE_VERSION) {
            ESP_LOGW(TAG, "Stored profile is version/layout %u (%u bytes, expected %u) "
                          "— ignoring it and using defaults",
                     (unsigned)s_profile->version, (unsigned)len,
                     (unsigned)sizeof(*s_profile));
            err = ESP_ERR_INVALID_VERSION;
        } else {
            buddy_profile_compute_hash(s_profile, s_profile->profile_hash);
            ESP_LOGI(TAG, "Profile loaded: name=%s", s_profile->display_name);
        }
    }

    /* Privacy */
    uint8_t priv = BUDDY_MODE_PUBLIC;
    size_t plen = sizeof(priv);
    nvs_get_blob(nvs, BUDDY_NVS_KEY_PRIV, &priv, &plen);
    s_privacy = (buddy_privacy_mode_t)priv;

    nvs_close(nvs);
    return err;
}

/* ── Default profile ──────────────────────────────────────────── */
static void profile_set_default(buddy_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->version = BUDDY_PROFILE_VERSION;
    snprintf(p->display_name, sizeof(p->display_name), "Buddy");
    snprintf(p->bio, sizeof(p->bio), "Exploring the world with Buddy.");
    /* Left empty on purpose: these are personal, and the chat prompt simply
     * omits whatever the owner has not filled in. */
    buddy_profile_compute_hash(p, p->profile_hash);
}

/* ═══════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════ */

esp_err_t buddy_profile_init(void)
{
    s_identity = heap_caps_calloc(1, sizeof(*s_identity), MALLOC_CAP_SPIRAM);
    s_profile   = heap_caps_calloc(1, sizeof(*s_profile), MALLOC_CAP_SPIRAM);
    if (!s_identity || !s_profile) {
        ESP_LOGE(TAG, "Failed to allocate profile/identity in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = identity_load_or_generate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Identity init failed");
        return err;
    }

    err = profile_load();
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_VERSION) {
            ESP_LOGW(TAG, "Stored profile was written by an older layout — using "
                          "defaults. Open the config page once and save to restore it.");
        } else {
            ESP_LOGW(TAG, "No profile in NVS, using defaults");
        }
        profile_set_default(s_profile);
    }

    return ESP_OK;
}

esp_err_t buddy_profile_get(buddy_profile_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memcpy(out, s_profile, sizeof(*out));
    return ESP_OK;
}

esp_err_t buddy_profile_set(const buddy_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;

    memcpy(s_profile, profile, sizeof(*s_profile));
    /* Whatever the caller handed over, what goes to NVS is this layout. */
    s_profile->version = BUDDY_PROFILE_VERSION;
    buddy_profile_compute_hash(s_profile, s_profile->profile_hash);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUDDY_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(nvs, BUDDY_NVS_KEY_PROF, s_profile, sizeof(*s_profile));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Profile saved: name=%s hash=%02x%02x...%02x",
             s_profile->display_name,
             s_profile->profile_hash[0], s_profile->profile_hash[1],
             s_profile->profile_hash[7]);
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
