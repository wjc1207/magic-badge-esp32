#include "wifi_onboard.h"
#include "onboard_html.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "buddy/buddy.h"
#include "buddy/buddy_profile.h"
#include "sdkconfig.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "onboard";
static httpd_handle_t s_server = NULL;
static bool s_captive_mode = false;

static void json_add_effective_config(cJSON *root, const char *json_key,
                                      const char *ns, const char *nvs_key,
                                      const char *build_val)
{
    char value[256] = {0};
    bool found = false;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(value);
        if (nvs_get_str(nvs, nvs_key, value, &len) == ESP_OK) {
            found = true;
        }
        nvs_close(nvs);
    }

    if (!found && build_val) {
        strlcpy(value, build_val, sizeof(value));
    }

    cJSON_AddStringToObject(root, json_key, value);
}

static void json_add_effective_config_u16(cJSON *root, const char *json_key,
                                          const char *ns, const char *nvs_key,
                                          const char *build_val)
{
    char value[16] = {0};
    bool found = false;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t port = 0;
        if (nvs_get_u16(nvs, nvs_key, &port) == ESP_OK && port > 0) {
            snprintf(value, sizeof(value), "%u", (unsigned)port);
            found = true;
        }
        nvs_close(nvs);
    }

    if (!found && build_val) {
        strlcpy(value, build_val, sizeof(value));
    }

    cJSON_AddStringToObject(root, json_key, value);
}

/* Milliseconds from the firmware, minutes for the form.
 *
 * A whole number of minutes is emitted without a decimal point ("3"), because
 * that is what the number input expects to display back and what a person
 * types.  The 0.2-minute floor is the firmware's own minimum, not a display
 * rounding. */
static void json_add_effective_config_minutes(cJSON *root, const char *json_key,
                                              int64_t ms)
{
    double minutes = (double)ms / 60000.0;
    char value[16];

    if (minutes == (double)(int64_t)minutes) {
        snprintf(value, sizeof(value), "%lld", (long long)minutes);
    } else {
        snprintf(value, sizeof(value), "%.2f", minutes);
    }

    cJSON_AddStringToObject(root, json_key, value);
}

static void json_add_effective_config_bool(cJSON *root, const char *json_key,
                                           const char *ns, const char *nvs_key,
                                           bool build_val)
{    bool value = build_val;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t bool_val = 0;
        if (nvs_get_u8(nvs, nvs_key, &bool_val) == ESP_OK) {
            value = bool_val ? true : false;
        }
        nvs_close(nvs);
    }

    cJSON_AddBoolToObject(root, json_key, value);
}

static bool get_feature_bool(const char *nvs_key, bool default_val)
{
    bool value = default_val;

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEATURE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t bool_val = 0;
        if (nvs_get_u8(nvs, nvs_key, &bool_val) == ESP_OK) {
            value = bool_val ? true : false;
        }
        nvs_close(nvs);
    }

    return value;
}

static esp_err_t set_feature_bool(const char *nvs_key, bool value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_FEATURE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, nvs_key, value ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static const char *get_feature_str(const char *nvs_key, const char *default_val)
{
    static char value[18] = {0};
    strlcpy(value, default_val, sizeof(value));

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEATURE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(value);
        if (nvs_get_str(nvs, nvs_key, value, &len) == ESP_OK) {
        }
        nvs_close(nvs);
    }

    return value;
}

static esp_err_t set_feature_str(const char *nvs_key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_FEATURE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, nvs_key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t set_feature_i32(const char *nvs_key, int value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_FEATURE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(nvs, nvs_key, (int32_t)value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

bool mimi_feature_telegram_bot_enabled(void)
{
    return get_feature_bool(MIMI_NVS_KEY_TELEGRAM_BOT, MIMI_FEATURE_TELEGRAM_BOT);
}

bool mimi_feature_feishu_bot_enabled(void)
{
    return get_feature_bool(MIMI_NVS_KEY_FEISHU_BOT, MIMI_FEATURE_FEISHU_BOT);
}

const char *mimi_ble_target_addr(void)
{
    return get_feature_str(MIMI_NVS_KEY_BLE_TARGET_ADDR, MIMI_BLE_TARGET_ADDR);
}

esp_err_t mimi_set_cam_xclk_freq(int freq)
{
    return set_feature_i32(MIMI_NVS_KEY_CAM_XCLK_FREQ, freq);
}

esp_err_t mimi_set_feature_telegram_bot(bool enabled)
{
    return set_feature_bool(MIMI_NVS_KEY_TELEGRAM_BOT, enabled);
}

esp_err_t mimi_set_feature_feishu_bot(bool enabled)
{
    return set_feature_bool(MIMI_NVS_KEY_FEISHU_BOT, enabled);
}

esp_err_t mimi_set_ble_target_addr(const char *addr)
{
    return set_feature_str(MIMI_NVS_KEY_BLE_TARGET_ADDR, addr);
}



/* ── DNS hijack ─────────────────────────────────────────────────── */

/* Minimal DNS response: always answer 192.168.4.1 */
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket error");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack listening on :53");

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len;

    while (1) {
        client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* too short for DNS header */

        /* Build response: copy query, set response flags, append answer */
        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;

        memcpy(resp, buf, len);

        /* Set QR=1 (response), AA=1 (authoritative), RA=1 */
        resp[2] = 0x85;  /* QR=1, Opcode=0, AA=1, TC=0, RD=1 */
        resp[3] = 0x80;  /* RA=1 */
        /* ANCOUNT = 1 */
        resp[6] = 0x00;
        resp[7] = 0x01;

        size_t off = (size_t)len;
        /* NAME: pointer to original QNAME at 0x0c */
        resp[off++] = 0xC0;
        resp[off++] = 0x0C;
        /* TYPE=A, CLASS=IN */
        resp[off++] = 0x00;
        resp[off++] = 0x01;
        resp[off++] = 0x00;
        resp[off++] = 0x01;
        /* TTL=0 */
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        resp[off++] = 0x00;
        /* RDLENGTH=4 */
        resp[off++] = 0x00;
        resp[off++] = 0x04;
        /* Append answer: pointer to name + A record with 192.168.4.1 */
        resp[off++] = 192; resp[off++] = 168;
        resp[off++] = 4;   resp[off++] = 1;
        sendto(sock, resp, off, 0,
               (struct sockaddr *)&client, client_len);
    }
}

/* ── HTTP handlers ──────────────────────────────────────────────── */

static esp_err_t http_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, ONBOARD_HTML, sizeof(ONBOARD_HTML) - 1);
}

/* Captive portal detection endpoints → redirect to root */
static esp_err_t http_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_get_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MIMI_ONBOARD_MAX_SCAN) ap_count = MIMI_ONBOARD_MAX_SCAN;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    uint16_t ap_max = ap_count;
    esp_wifi_scan_get_ap_records(&ap_max, ap_list);

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_max; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;  /* skip hidden */
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(obj, "ch", ap_list[i].primary);
        cJSON_AddBoolToObject(obj, "auth", ap_list[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, obj);
    }
    free(ap_list);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t http_get_config(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    json_add_effective_config(root, "ssid", MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID, MIMI_SECRET_WIFI_SSID);
    json_add_effective_config(root, "password", MIMI_NVS_WIFI, MIMI_NVS_KEY_PASS, MIMI_SECRET_WIFI_PASS);
    json_add_effective_config(root, "api_key", MIMI_NVS_LLM, MIMI_NVS_KEY_API_KEY, MIMI_SECRET_API_KEY);
    json_add_effective_config(root, "model", MIMI_NVS_LLM, MIMI_NVS_KEY_MODEL, MIMI_SECRET_MODEL);
    json_add_effective_config(root, "provider", MIMI_NVS_LLM, MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER);
    json_add_effective_config(root, "system_prompt", MIMI_NVS_LLM, MIMI_NVS_KEY_SYSTEM_PROMPT, "");
    json_add_effective_config(root, "tg_token", MIMI_NVS_TG, MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN);
    json_add_effective_config(root, "feishu_app_id", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, MIMI_SECRET_FEISHU_APP_ID);
    json_add_effective_config(root, "feishu_app_secret", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET, MIMI_SECRET_FEISHU_APP_SECRET);
    json_add_effective_config(root, "proxy_host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST);
    json_add_effective_config_u16(root, "proxy_port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT);
    json_add_effective_config(root, "proxy_type", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_TYPE, MIMI_SECRET_PROXY_TYPE);
    json_add_effective_config(root, "search_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY, MIMI_SECRET_SEARCH_KEY);
    json_add_effective_config(root, "tavily_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY, MIMI_SECRET_TAVILY_KEY);

    /* Feature toggles */
    json_add_effective_config_bool(root, "telegram_bot", MIMI_NVS_FEATURE, MIMI_NVS_KEY_TELEGRAM_BOT, MIMI_FEATURE_TELEGRAM_BOT);
    json_add_effective_config_bool(root, "feishu_bot", MIMI_NVS_FEATURE, MIMI_NVS_KEY_FEISHU_BOT, MIMI_FEATURE_FEISHU_BOT);

    /* Buddy profile.
     *
     * On the heap, not the stack. This struct is about 5.6 KB — bio 2048 +
     * scene 1280 + appearance 1024 + belongings 1024 + knows 512 + the rest —
     * and the HTTP server task has an 8 KB stack. Declaring it locally left
     * under 3 KB for the cJSON calls in this function and for the several frames
     * NVS pushes when it reads, and the board crashed with a LoadProhibited
     * inside esp_flash_read while serving this very request.
     *
     * It is read-only here and freed before returning, so PSRAM is the right
     * place for it: internal RAM is the scarce resource on this board, and the
     * profile is only ever touched a few times per page load. */
    buddy_profile_t *bp = heap_caps_calloc(1, sizeof(buddy_profile_t), MALLOC_CAP_SPIRAM);
    if (bp) {
        esp_err_t bp_err = buddy_profile_get(bp);
        if (bp_err == ESP_OK) {
            cJSON_AddStringToObject(root, "buddy_name", bp->display_name);
            cJSON_AddStringToObject(root, "buddy_bio", bp->bio);
            cJSON_AddStringToObject(root, "buddy_appearance", bp->appearance);
            cJSON_AddStringToObject(root, "buddy_belongings", bp->belongings);
            cJSON_AddStringToObject(root, "buddy_traits", bp->traits);
            cJSON_AddStringToObject(root, "buddy_tech_level", bp->tech_level);
            cJSON_AddStringToObject(root, "buddy_speech", bp->speech);
            cJSON_AddStringToObject(root, "buddy_knows", bp->knows);
            /* The preset scene, sent with the card so the page can show and edit
             * it. It is not part of the character: a scene belongs to an
             * encounter, and the two badges each keep their own copy rather than
             * exchanging one. */
            cJSON_AddStringToObject(root, "buddy_scene", bp->scene);
            /* The scene's label, which names the exported file. Local only: it
             * never goes over BLE and is not part of the prompt. */
            cJSON_AddStringToObject(root, "buddy_scene_name", bp->scene_name);
        }
        heap_caps_free(bp);
    }
    cJSON_AddBoolToObject(root, "buddy_privacy",
                          buddy_privacy_get() == BUDDY_MODE_PRIVATE);

    /* Cool-downs: the form works in minutes, the firmware in milliseconds. */
    json_add_effective_config_minutes(root, "buddy_cool_peer_min",
                                      buddy_cooldown_peer_ms());
    json_add_effective_config_minutes(root, "buddy_cool_session_min",
                                      buddy_cooldown_session_ms());

    /* Where the last conversation came from — the config page shows it, and it
     * is also the destination every BLE-chat line is forwarded to. */
    {
        char value[128] = {0};
        nvs_handle_t nvs;
        if (nvs_open(MIMI_NVS_FEATURE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(value);
            if (nvs_get_str(nvs, MIMI_NVS_KEY_LAST_SRC_CHANNEL, value, &len) == ESP_OK) {
                cJSON_AddStringToObject(root, "last_src_channel", value);
            }
            len = sizeof(value); value[0] = '\0';
            if (nvs_get_str(nvs, MIMI_NVS_KEY_LAST_SRC_CHAT_ID, value, &len) == ESP_OK) {
                cJSON_AddStringToObject(root, "last_src_chat_id", value);
            }
            nvs_close(nvs);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

/*
 * Sync one JSON string field into NVS.
 * - missing field: leave current NVS value unchanged
 * - empty string: erase current NVS value
 * - non-empty string: save/update current NVS value
 */
static void nvs_sync_field(cJSON *root, const char *json_key,
                           const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (item->valuestring[0] == '\0') {
            esp_err_t err = nvs_erase_key(nvs, nvs_key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
            }
        } else {
            nvs_set_str(nvs, nvs_key, item->valuestring);
            ESP_LOGI(TAG, "Saved %s/%s", ns, nvs_key);
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void nvs_sync_u16_field(cJSON *root, const char *json_key,
                               const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || (!cJSON_IsString(item) && !cJSON_IsNumber(item))) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (cJSON_IsString(item) && item->valuestring[0] == '\0') {
            esp_err_t err = nvs_erase_key(nvs, nvs_key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
            }
        } else {
            uint16_t value = 0;
            bool valid = true;

            if (cJSON_IsString(item)) {
                char *end = NULL;
                unsigned long ul_value = strtoul(item->valuestring, &end, 10);
                if (end == item->valuestring || *end != '\0' || ul_value > UINT16_MAX) {
                    ESP_LOGW(TAG, "Ignoring invalid %s value: %s", json_key, item->valuestring);
                    valid = false;
                } else {
                    value = (uint16_t)ul_value;
                }
            } else if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0 || item->valuedouble > UINT16_MAX) {
                    ESP_LOGW(TAG, "Ignoring invalid %s value: %f", json_key, item->valuedouble);
                    valid = false;
                } else {
                    value = (uint16_t)item->valuedouble;
                }
            }

            if (valid) {
                ESP_ERROR_CHECK(nvs_set_u16(nvs, nvs_key, value));
                ESP_LOGI(TAG, "Saved %s/%s", ns, nvs_key);
            }
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void nvs_sync_bool_field(cJSON *root, const char *json_key,
                               const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (cJSON_IsBool(item)) {
            ESP_ERROR_CHECK(nvs_set_u8(nvs, nvs_key, item->valueint ? 1 : 0));
            ESP_LOGI(TAG, "Saved %s/%s: %s", ns, nvs_key, item->valueint ? "true" : "false");
        } else if (cJSON_IsString(item)) {
            if (item->valuestring[0] == '\0') {
                esp_err_t err = nvs_erase_key(nvs, nvs_key);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
                } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                    ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
                }
            } else {
                bool value = (strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0);
                ESP_ERROR_CHECK(nvs_set_u8(nvs, nvs_key, value ? 1 : 0));
                ESP_LOGI(TAG, "Saved %s/%s: %s", ns, nvs_key, value ? "true" : "false");
            }
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t http_post_save(httpd_req_t *req)
{
    int total_len = req->content_len;
    /* The body is the whole form, and the character fields alone are allowed to
     * total about 5.3 KB: bio 2047 + appearance 1023 + belongings 1023 +
     * knows 511 + traits/speech 510 + tech_level 127 + name 31, plus the JSON
     * keys, the WiFi/LLM/bot fields and escaping.  4096 rejected a fully filled
     * form with "Bad length" and saved nothing at all — a silent failure that
     * looked like the page being broken.
     *
     * The scene adds up to another 1.2 KB on top of that, and it is meant to be
     * filled in at the length the sandbox experiments found useful — a scene
     * carrying twenty-odd nameable things runs to 300-400 characters, which is
     * over 1 KB in UTF-8.  12 KB keeps every field able to be filled completely
     * with room for the JSON scaffolding around them.
     *
     * The cap is still a cap: it is what stops a hostile or malformed POST from
     * asking for an arbitrary allocation. */
    if (total_len <= 0 || total_len > 12288) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_FAIL;
    }

    char *buf = calloc(1, total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv error");
            return ESP_FAIL;
        }
        received += ret;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    /* WiFi (required) */
    nvs_sync_field(root, "ssid",     MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID);
    nvs_sync_field(root, "password", MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS);

    /* LLM */
    nvs_sync_field(root, "api_key",  MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY);
    nvs_sync_field(root, "model",    MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL);
    nvs_sync_field(root, "provider", MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER);
    nvs_sync_field(root, "system_prompt", MIMI_NVS_LLM, MIMI_NVS_KEY_SYSTEM_PROMPT);

    /* Telegram */
    nvs_sync_field(root, "tg_token", MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN);

    /* Feishu */
    nvs_sync_field(root, "feishu_app_id",     MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID);
    nvs_sync_field(root, "feishu_app_secret", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET);

    /* Proxy */
    nvs_sync_field(root, "proxy_host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST);
    nvs_sync_u16_field(root, "proxy_port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT);
    nvs_sync_field(root, "proxy_type", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_TYPE);

    /* Search */
    nvs_sync_field(root, "search_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY);
    nvs_sync_field(root, "tavily_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY);

    /* Feature toggles */
    nvs_sync_bool_field(root, "telegram_bot", MIMI_NVS_FEATURE, MIMI_NVS_KEY_TELEGRAM_BOT);
    nvs_sync_bool_field(root, "feishu_bot", MIMI_NVS_FEATURE, MIMI_NVS_KEY_FEISHU_BOT);

    /* Buddy profile. Heap for the same reason as the GET path: this struct is
     * 5.6 KB and the HTTP task has 8 KB of stack. */
    {
        buddy_profile_t *bp = heap_caps_calloc(1, sizeof(buddy_profile_t),
                                               MALLOC_CAP_SPIRAM);
        if (!bp) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
            return ESP_FAIL;
        }
        buddy_profile_get(bp);

        cJSON *item;
        #define SYNC_BUDDY_FIELD(json_key, dest, maxlen) \
            item = cJSON_GetObjectItem(root, json_key); \
            if (item && cJSON_IsString(item)) { \
                snprintf(dest, maxlen, "%s", item->valuestring); \
            }

        SYNC_BUDDY_FIELD("buddy_name", bp->display_name, sizeof(bp->display_name));
        SYNC_BUDDY_FIELD("buddy_bio", bp->bio, sizeof(bp->bio));
        SYNC_BUDDY_FIELD("buddy_appearance", bp->appearance, sizeof(bp->appearance));
        SYNC_BUDDY_FIELD("buddy_belongings", bp->belongings, sizeof(bp->belongings));
        SYNC_BUDDY_FIELD("buddy_traits", bp->traits, sizeof(bp->traits));
        SYNC_BUDDY_FIELD("buddy_tech_level", bp->tech_level, sizeof(bp->tech_level));
        SYNC_BUDDY_FIELD("buddy_speech", bp->speech, sizeof(bp->speech));
        SYNC_BUDDY_FIELD("buddy_knows", bp->knows, sizeof(bp->knows));
        SYNC_BUDDY_FIELD("buddy_scene", bp->scene, sizeof(bp->scene));
        SYNC_BUDDY_FIELD("buddy_scene_name", bp->scene_name, sizeof(bp->scene_name));

        buddy_profile_set(bp);
        heap_caps_free(bp);

        /* Privacy mode */
        cJSON *priv = cJSON_GetObjectItem(root, "buddy_privacy");
        if (priv) {
            bool is_private = false;
            if (cJSON_IsBool(priv)) is_private = priv->valueint;
            else if (cJSON_IsString(priv)) {
                is_private = (strcmp(priv->valuestring, "true") == 0 ||
                              strcmp(priv->valuestring, "1") == 0);
            }
            buddy_privacy_set(is_private ? BUDDY_MODE_PRIVATE : BUDDY_MODE_PUBLIC);
        }

        /* Cool-downs.  The form sends minutes and may send them as strings ("3")
         * or numbers (3), so accept both; a blank field leaves the stored value
         * alone rather than reading as "zero".  buddy_cooldowns_set() clamps
         * whatever arrives to the firmware's guard rails, so an out-of-range
         * number is corrected rather than rejected. */
        int64_t cool_peer = -1, cool_sess = -1;

        cJSON *cp = cJSON_GetObjectItem(root, "buddy_cool_peer_min");
        if (cJSON_IsNumber(cp)) cool_peer = (int64_t)(cp->valuedouble * 60000.0);
        else if (cJSON_IsString(cp) && cp->valuestring[0])
            cool_peer = (int64_t)(atof(cp->valuestring) * 60000.0);

        cJSON *cs = cJSON_GetObjectItem(root, "buddy_cool_session_min");
        if (cJSON_IsNumber(cs)) cool_sess = (int64_t)(cs->valuedouble * 60000.0);
        else if (cJSON_IsString(cs) && cs->valuestring[0])
            cool_sess = (int64_t)(atof(cs->valuestring) * 60000.0);

        if (cool_peer > 0 || cool_sess > 0) {
            buddy_cooldowns_set(cool_peer > 0 ? cool_peer : buddy_cooldown_peer_ms(),
                                cool_sess > 0 ? cool_sess : buddy_cooldown_session_ms());
        }
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);

    ESP_LOGI(TAG, "Configuration saved, restarting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

/* ── Soft AP + HTTP server startup ──────────────────────────────── */

static esp_err_t start_softap(bool keep_sta)
{
    /* Get last 2 bytes of MAC for unique SSID suffix */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X", MIMI_ONBOARD_AP_PREFIX, mac[4], mac[5]);

    /* Create AP netif if not already present */
    static esp_netif_t *ap_netif = NULL;
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* APSTA lets the local config AP coexist with WiFi scanning/STA usage. */
    (void)keep_sta;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    const char *ap_pass = MIMI_ONBOARD_AP_PASS;
    size_t pass_len = strlen(ap_pass);
    bool use_secure_ap = (pass_len >= 8 && pass_len <= 63);

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode = use_secure_ap ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    if (use_secure_ap) {
        strncpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password) - 1);
    } else {
        ESP_LOGW(TAG, "MIMI_ONBOARD_AP_PASS is invalid (need 8-63 chars), fallback to open AP");
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && !(keep_sta && err == ESP_ERR_WIFI_CONN)) {
        return err;
    }

    ESP_LOGI(TAG, "Soft AP started: %s (%s)", ssid, use_secure_ap ? "WPA2" : "open");
    return ESP_OK;
}

static httpd_handle_t start_http_server(bool captive)
{
    if (s_server) {
        if (captive && !s_captive_mode) {
            ESP_LOGW(TAG, "HTTP server already running without captive redirects");
        }
        return s_server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_ONBOARD_HTTP_PORT;
    config.max_uri_handlers = captive ? 16 : 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    /* Main page */
    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = http_get_root,
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_config = {
        .uri = "/config", .method = HTTP_GET, .handler = http_get_config,
    };
    httpd_register_uri_handler(s_server, &uri_config);

    /* WiFi scan */
    httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = http_get_scan,
    };
    httpd_register_uri_handler(s_server, &uri_scan);

    /* Save config */
    httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = http_post_save,
    };
    httpd_register_uri_handler(s_server, &uri_save);

    if (captive) {
        /* Captive portal detection endpoints */
        const char *captive_uris[] = {
            "/generate_204",           /* Android */
            "/gen_204",                /* Android alt */
            "/hotspot-detect.html",    /* iOS/macOS */
            "/library/test/success.html", /* iOS alt */
            "/connecttest.txt",        /* Windows */
            "/redirect",               /* Windows alt */
        };
        for (int i = 0; i < sizeof(captive_uris) / sizeof(captive_uris[0]); i++) {
            httpd_uri_t uri_captive = {
                .uri = captive_uris[i],
                .method = HTTP_GET,
                .handler = http_captive_redirect,
            };
            httpd_register_uri_handler(s_server, &uri_captive);
        }
    }

    s_captive_mode = captive;
    ESP_LOGI(TAG, "HTTP server started on port %d", MIMI_ONBOARD_HTTP_PORT);
    return s_server;
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t wifi_onboard_start(wifi_onboard_mode_t mode)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting WiFi Configuration Portal");
    ESP_LOGI(TAG, "========================================");

    bool captive = (mode == WIFI_ONBOARD_MODE_CAPTIVE);
    if (captive) {
        /* Stop STA retries before starting captive portal. */
        wifi_manager_set_reconnect_enabled(false);
        wifi_manager_stop();
    }

    /* Start soft AP */
    esp_err_t err = start_softap(!captive);
    if (err != ESP_OK) return err;

    if (captive) {
        /* Start DNS hijack only for true captive portal mode. */
        xTaskCreate(dns_hijack_task, "dns_hijack",
                    MIMI_ONBOARD_DNS_STACK, NULL, 5, NULL);
    }

    /* Start HTTP server */
    httpd_handle_t server = start_http_server(captive);
    if (!server) return ESP_FAIL;

    ESP_LOGI(TAG, "Connect to MagicBadge-XXXX WiFi, then open http://192.168.4.1");

    if (!captive) {
        ESP_LOGI(TAG, "Local admin portal stays available while STA is connected");
        return ESP_OK;
    }

    /* Block forever — onboarding ends with esp_restart() in /save handler */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_OK;  /* unreachable */
}
