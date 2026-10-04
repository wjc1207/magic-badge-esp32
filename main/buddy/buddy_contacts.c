#include "buddy_contacts.h"
#include "mimi_config.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "buddy_contacts";
#define BUDDY_CONTACTS_FILE  MIMI_SPIFFS_BASE "/contacts.json"

/* Simple header-based array storage in SPIFFS as JSON */
static esp_err_t contacts_read_all(cJSON **out)
{
    FILE *f = fopen(BUDDY_CONTACTS_FILE, "r");
    if (!f) {
        *out = cJSON_CreateArray();
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) {
        fclose(f);
        *out = cJSON_CreateArray();
        return ESP_OK;
    }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);

    *out = cJSON_Parse(buf);
    free(buf);
    if (!*out) *out = cJSON_CreateArray();
    return ESP_OK;
}

static esp_err_t contacts_write_all(cJSON *arr)
{
    char *json = cJSON_PrintUnformatted(arr);
    if (!json) return ESP_ERR_NO_MEM;

    FILE *f = fopen(BUDDY_CONTACTS_FILE, "w");
    if (!f) { free(json); return ESP_FAIL; }
    fputs(json, f);
    fclose(f);
    free(json);
    return ESP_OK;
}

static int64_t unix_now(void)
{
    time_t t;
    time(&t);
    return (int64_t)t;
}

/* ── Upsert ───────────────────────────────────────────────────── */
esp_err_t buddy_contacts_upsert(const buddy_contact_record_t *rec)
{
    cJSON *arr = NULL;
    contacts_read_all(&arr);
    if (!arr) return ESP_FAIL;

    /* Find existing entry by peer_id */
    cJSON *existing = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *pid = cJSON_GetObjectItem(item, "peer_id");
        if (pid && cJSON_IsString(pid) && strcmp(pid->valuestring, rec->peer_id) == 0) {
            existing = item;
            break;
        }
    }

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "peer_id", rec->peer_id);
    cJSON_AddStringToObject(entry, "appearance", rec->appearance);
    cJSON_AddStringToObject(entry, "belongings", rec->belongings);
    cJSON_AddNumberToObject(entry, "last_met_unix", (double)unix_now());
    cJSON_AddNumberToObject(entry, "meeting_count", rec->meeting_count + 1);

    if (existing) {
        /* Find the index of existing item in the array */
        int idx = 0;
        cJSON *iter = NULL;
        cJSON_ArrayForEach(iter, arr) {
            if (iter == existing) break;
            idx++;
        }
        cJSON_DeleteItemFromArray(arr, idx);
    }

    /* Add to front (most recent first) */
    cJSON_AddItemToArray(arr, entry);

    /* Trim to max size */
    int sz = cJSON_GetArraySize(arr);
    while (sz > BUDDY_MAX_CONTACTS) {
        cJSON_DeleteItemFromArray(arr, sz - 1);
        sz--;
    }

    esp_err_t ret = contacts_write_all(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Contact upserted: %s (total=%d)", rec->peer_id, sz);
    return ret;
}

/* ── Get ──────────────────────────────────────────────────────── */
esp_err_t buddy_contacts_get(const char *peer_id, buddy_contact_record_t *out)
{
    cJSON *arr = NULL;
    contacts_read_all(&arr);
    if (!arr) return ESP_FAIL;

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *pid = cJSON_GetObjectItem(item, "peer_id");
        if (pid && cJSON_IsString(pid) && strcmp(pid->valuestring, peer_id) == 0) {
            memset(out, 0, sizeof(*out));
            snprintf(out->peer_id, sizeof(out->peer_id), "%s",
                     cJSON_GetObjectItem(item, "peer_id")->valuestring);
            cJSON *ap = cJSON_GetObjectItem(item, "appearance");
            if (ap) snprintf(out->appearance, sizeof(out->appearance), "%s", ap->valuestring);
            cJSON *bl = cJSON_GetObjectItem(item, "belongings");
            if (bl) snprintf(out->belongings, sizeof(out->belongings), "%s", bl->valuestring);
            cJSON *lm = cJSON_GetObjectItem(item, "last_met_unix");
            if (lm) out->last_met_unix = (int64_t)lm->valuedouble;
            cJSON *mc = cJSON_GetObjectItem(item, "meeting_count");
            if (mc) out->meeting_count = (uint16_t)mc->valueint;
            ret = ESP_OK;
            break;
        }
    }

    cJSON_Delete(arr);
    return ret;
}

/* ── List ─────────────────────────────────────────────────────── */
esp_err_t buddy_contacts_list(buddy_contact_record_t *buf, size_t max, size_t *count)
{
    if (!buf || !max || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    cJSON *arr = NULL;
    contacts_read_all(&arr);
    if (!arr) return ESP_FAIL;

    cJSON *item = NULL;
    size_t i = 0;
    cJSON_ArrayForEach(item, arr) {
        if (i >= max) break;
        cJSON *pid = cJSON_GetObjectItem(item, "peer_id");
        if (pid && cJSON_IsString(pid)) {
            buddy_contacts_get(pid->valuestring, &buf[i]);
            i++;
        }
    }

    *count = i;
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ── Check status ─────────────────────────────────────────────── */
buddy_contact_status_t buddy_contacts_check(const char *peer_id)
{
    buddy_contact_record_t *rec = heap_caps_calloc(1, sizeof(*rec), MALLOC_CAP_SPIRAM);
    if (!rec) return BUDDY_CONTACT_NEW;

    esp_err_t err = buddy_contacts_get(peer_id, rec);
    if (err == ESP_ERR_NOT_FOUND) {
        heap_caps_free(rec);
        return BUDDY_CONTACT_NEW;
    }

    int64_t now = unix_now();
    int64_t age = now - rec->last_met_unix;
    heap_caps_free(rec);
    if (age < 86400) return BUDDY_CONTACT_RECENT;
    return BUDDY_CONTACT_KNOWN;
}

/* ── Init ─────────────────────────────────────────────────────── */
esp_err_t buddy_contacts_init(void)
{
    /* Ensure contacts file exists */
    FILE *f = fopen(BUDDY_CONTACTS_FILE, "r");
    if (!f) {
        f = fopen(BUDDY_CONTACTS_FILE, "w");
        if (f) {
            fputs("[]", f);
            fclose(f);
        }
    } else {
        fclose(f);
    }
    ESP_LOGI(TAG, "Contact store ready at %s", BUDDY_CONTACTS_FILE);
    return ESP_OK;
}
