#include "llm_proxy.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "llm";

#define LLM_API_KEY_MAX_LEN 320
#define LLM_MODEL_MAX_LEN   64
#define LLM_DUMP_MAX_BYTES   (16 * 1024)
#define LLM_DUMP_CHUNK_BYTES 320
#define LLM_HTTP_BUFFER_RX    1024
#define LLM_HTTP_BUFFER_TX    512

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN] = MIMI_LLM_DEFAULT_MODEL;
static char s_provider[16] = MIMI_LLM_PROVIDER_DEFAULT;

typedef enum {
    LLM_PROVIDER_ANTHROPIC = 0,
    LLM_PROVIDER_OPENAI,
    LLM_PROVIDER_OPENROUTER,
    LLM_PROVIDER_NVIDIA,
    LLM_PROVIDER_QWEN,
    LLM_PROVIDER_DEEPSEEK,
    LLM_PROVIDER_UNKNOWN,
} llm_provider_t;

typedef struct {
    const char    *name;
    llm_provider_t id;
    const char    *url;
    const char    *host;
    const char    *path;
} provider_map_t;

static const provider_map_t k_provider_map[] = {
    { "anthropic",  LLM_PROVIDER_ANTHROPIC,  MIMI_LLM_API_URL,        "api.anthropic.com",        "/v1/messages"         },
    { "openai",     LLM_PROVIDER_OPENAI,     MIMI_OPENAI_API_URL,     "api.openai.com",           "/v1/chat/completions" },
    { "openrouter", LLM_PROVIDER_OPENROUTER, MIMI_OPENROUTER_API_URL, "openrouter.ai",            "/v1/chat/completions" },
    { "nvidia",     LLM_PROVIDER_NVIDIA,     MIMI_NVIDIA_API_URL,     "integrate.api.nvidia.com", "/v1/chat/completions" },
    { "qwen",       LLM_PROVIDER_QWEN,       MIMI_QWEN_API_URL,       "dashscope.aliyuncs.com",   "/compatible-mode/v1/chat/completions" },
    { "deepseek",   LLM_PROVIDER_DEEPSEEK,   MIMI_DEEPSEEK_API_URL,  "api.deepseek.com",         "/v1/chat/completions"           },
};
#define PROVIDER_MAP_LEN (sizeof(k_provider_map) / sizeof(k_provider_map[0]))

static llm_provider_t s_llm_provider = LLM_PROVIDER_ANTHROPIC;

static llm_provider_t provider_parse(const char *str)
{
    if (!str) return LLM_PROVIDER_UNKNOWN;
    for (size_t i = 0; i < PROVIDER_MAP_LEN; i++) {
        if (strcasecmp(str, k_provider_map[i].name) == 0) {
            return k_provider_map[i].id;
        }
    }
    return LLM_PROVIDER_UNKNOWN;
}

static const provider_map_t *provider_entry(void)
{
    for (size_t i = 0; i < PROVIDER_MAP_LEN; i++) {
        if (k_provider_map[i].id == s_llm_provider) {
            return &k_provider_map[i];
        }
    }
    return &k_provider_map[0]; /* defensive fallback to anthropic */
}

bool llm_provider_is_anthropic(void)
{
    return s_llm_provider == LLM_PROVIDER_ANTHROPIC;
}

static void llm_log_payload(const char *label, const char *payload)
{
    if (!payload) {
        ESP_LOGI(TAG, "%s: <null>", label);
        return;
    }

    size_t total = strlen(payload);
#if MIMI_LLM_LOG_VERBOSE_PAYLOAD
    size_t shown = total > LLM_DUMP_MAX_BYTES ? LLM_DUMP_MAX_BYTES : total;
    ESP_LOGI(TAG, "%s (%u bytes)%s",
             label,
             (unsigned)total,
             (shown < total) ? " [truncated]" : "");

    char chunk[LLM_DUMP_CHUNK_BYTES + 1];
    for (size_t off = 0; off < shown; off += LLM_DUMP_CHUNK_BYTES) {
        size_t n = shown - off;
        if (n > LLM_DUMP_CHUNK_BYTES) {
            n = LLM_DUMP_CHUNK_BYTES;
        }
        memcpy(chunk, payload + off, n);
        chunk[n] = '\0';
        ESP_LOGI(TAG, "%s[%u]: %s", label, (unsigned)off, chunk);
    }
#else
    if (MIMI_LLM_LOG_PREVIEW_BYTES > 0) {
        size_t shown = total > MIMI_LLM_LOG_PREVIEW_BYTES ? MIMI_LLM_LOG_PREVIEW_BYTES : total;
        char preview[MIMI_LLM_LOG_PREVIEW_BYTES + 1];
        memcpy(preview, payload, shown);
        preview[shown] = '\0';
        for (size_t i = 0; i < shown; i++) {
            if (preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t') {
                preview[i] = ' ';
            }
        }
        ESP_LOGI(TAG, "%s (%u bytes): %s%s",
                 label,
                 (unsigned)total,
                 preview,
                 (shown < total) ? " ..." : "");
    } else {
        ESP_LOGI(TAG, "%s (%u bytes)", label, (unsigned)total);
    }
#endif
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ── Response buffer ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    if (rb->len + len > MIMI_LLM_RESP_MAX_BYTES) {
        ESP_LOGE(TAG, "Response too large (>%u bytes), aborting", MIMI_LLM_RESP_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }
    while (rb->len + len >= rb->cap) {
        if (rb->cap > SIZE_MAX / 2) {
            ESP_LOGE(TAG, "Response buffer capacity overflow");
            return ESP_ERR_NO_MEM;
        }
        size_t new_cap = rb->cap * 2;
        char *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) return ESP_ERR_NO_MEM;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static void resp_buf_reset(resp_buf_t *rb)
{
    if (!rb || !rb->data) {
        return;
    }
    rb->len = 0;
    rb->data[0] = '\0';
}


/* ── Chunked transfer encoding decoder ───────────────────────── */

static void resp_buf_decode_chunked(resp_buf_t *rb)
{
    if (!rb->data || rb->len == 0) return;

    /* Quick check: if body starts with '{' or '[', it's not chunked */
    size_t i = 0;
    while (i < rb->len && (rb->data[i] == ' ' || rb->data[i] == '\t')) i++;
    if (i < rb->len && (rb->data[i] == '{' || rb->data[i] == '[')) return;

    /* Try to decode chunked encoding in-place */
    char *src = rb->data;
    char *dst = rb->data;
    char *end = rb->data + rb->len;

    while (src < end) {
        /* Parse hex chunk size */
        char *line_end = strstr(src, "\r\n");
        if (!line_end) break;

        unsigned long chunk_size = strtoul(src, NULL, 16);
        if (chunk_size == 0) break;  /* terminal chunk */

        src = line_end + 2;  /* skip past \r\n after size */

        if (src + chunk_size > end) {
            /* Incomplete chunk, copy what we have */
            size_t avail = end - src;
            memmove(dst, src, avail);
            dst += avail;
            break;
        }

        memmove(dst, src, chunk_size);
        dst += chunk_size;
        src += chunk_size;

        /* Skip trailing \r\n after chunk data */
        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n') {
            src += 2;
        }
    }

    rb->len = dst - rb->data;
    rb->data[rb->len] = '\0';
}

/* ── HTTP event handler (for esp_http_client direct path) ─────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp_buf_append(rb, (const char *)evt->data, evt->data_len) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* ── Provider helpers ──────────────────────────────────────────── */

static const char *llm_api_url(void)  { return provider_entry()->url;  }
static const char *llm_api_host(void) { return provider_entry()->host; }
static const char *llm_api_path(void) { return provider_entry()->path; }

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t llm_proxy_init(void)
{
    /* Start with build-time defaults */
    if (MIMI_SECRET_API_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_API_KEY);
    }
    if (MIMI_SECRET_MODEL[0] != '\0') {
        safe_copy(s_model, sizeof(s_model), MIMI_SECRET_MODEL);
    }
    if (MIMI_SECRET_MODEL_PROVIDER[0] != '\0') {
        safe_copy(s_provider, sizeof(s_provider), MIMI_SECRET_MODEL_PROVIDER);
        s_llm_provider = provider_parse(s_provider);
        if (s_llm_provider == LLM_PROVIDER_UNKNOWN) {
            ESP_LOGW(TAG, "Unknown provider '%s' in build config; defaulting to anthropic", s_provider);
            s_llm_provider = LLM_PROVIDER_ANTHROPIC;
        }
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[LLM_API_KEY_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            safe_copy(s_api_key, sizeof(s_api_key), tmp);
        }
        char model_tmp[LLM_MODEL_MAX_LEN] = {0};
        len = sizeof(model_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, model_tmp, &len) == ESP_OK && model_tmp[0]) {
            safe_copy(s_model, sizeof(s_model), model_tmp);
        }
        char provider_tmp[16] = {0};
        len = sizeof(provider_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_PROVIDER, provider_tmp, &len) == ESP_OK && provider_tmp[0]) {
            llm_provider_t parsed = provider_parse(provider_tmp);
            if (parsed == LLM_PROVIDER_UNKNOWN) {
                ESP_LOGW(TAG, "Unknown provider '%s' in NVS; keeping current provider", provider_tmp);
            } else {
                safe_copy(s_provider, sizeof(s_provider), provider_tmp);
                s_llm_provider = parsed;
            }
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "LLM proxy initialized (provider: %s, model: %s)", s_provider, s_model);
    } else {
        ESP_LOGW(TAG, "No API key. Use CLI: set_api_key <KEY>");
    }
    return ESP_OK;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static esp_err_t llm_http_direct(const char *post_data, resp_buf_t *rb, int *out_status)
{
    esp_http_client_config_t config = {
        .url = llm_api_url(),
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = 120 * 1000,
        .buffer_size = LLM_HTTP_BUFFER_RX,
        .buffer_size_tx = LLM_HTTP_BUFFER_TX,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_llm_provider != LLM_PROVIDER_ANTHROPIC) {
        if (s_api_key[0]) {
            char auth[LLM_API_KEY_MAX_LEN + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
            esp_http_client_set_header(client, "Authorization", auth);
        }
    } else {
        esp_http_client_set_header(client, "x-api-key", s_api_key);
        esp_http_client_set_header(client, "anthropic-version", MIMI_LLM_API_VERSION);
    }
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static esp_err_t llm_http_via_proxy(const char *post_data, resp_buf_t *rb, int *out_status)
{
    proxy_conn_t *conn = proxy_conn_open(llm_api_host(), 443, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    int body_len = strlen(post_data);
    char header[1024];
    int hlen = 0;
    if (s_llm_provider != LLM_PROVIDER_ANTHROPIC) {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            llm_api_path(), llm_api_host(), s_api_key, body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "x-api-key: %s\r\n"
            "anthropic-version: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            llm_api_path(), llm_api_host(), s_api_key, MIMI_LLM_API_VERSION, body_len);
    }

    if (hlen < 0 || (size_t)hlen >= sizeof(header)) {
        proxy_conn_close(conn);
        return ESP_ERR_INVALID_SIZE;
    }

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response into buffer */
    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
        if (n <= 0) break;
        if (resp_buf_append(rb, tmp, n) != ESP_OK) break;
    }
    proxy_conn_close(conn);

    /* Parse status line */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) *out_status = atoi(sp + 1);
    }

    /* Strip HTTP headers, keep body only */
    char *body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    /* Decode chunked transfer encoding if present */
    resp_buf_decode_chunked(rb);

    return ESP_OK;
}

/* ── Shared HTTP dispatch ─────────────────────────────────────── */

static esp_err_t llm_http_call(const char *post_data, resp_buf_t *rb, int *out_status)
{
    if (!http_proxy_http_lock(10000)) {
        ESP_LOGE(TAG, "HTTP lock timeout before LLM request");
        return ESP_ERR_TIMEOUT;
    }


    esp_err_t ret = ESP_FAIL;
    if (http_proxy_is_enabled()) {
        ret = llm_http_via_proxy(post_data, rb, out_status);
    } else {
        esp_err_t err = llm_http_direct(post_data, rb, out_status);
        if (err == ESP_OK) {
            ret = ESP_OK;
        } else if (err == ESP_FAIL || err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_NO_MEM || err == ESP_ERR_HTTP_WRITE_DATA) {
            ESP_LOGW(TAG,
                     "Transient HTTP failure (%s), retry once after backoff (internal=%u, psram=%u)",
                     esp_err_to_name(err),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            resp_buf_reset(rb);
            if (out_status) {
                *out_status = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            ret = llm_http_direct(post_data, rb, out_status);
        } else {
            ret = err;
        }
    }

    http_proxy_http_unlock();
    return ret;
}

/* ── Parse text from JSON response ────────────────────────────── */

static void extract_text_anthropic(cJSON *root, char *buf, size_t size)
{
    if (!buf || size < 2) return;
    buf[0] = '\0';
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!content || !cJSON_IsArray(content)) return;

    size_t off = 0;
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        cJSON *btype = cJSON_GetObjectItem(block, "type");
        if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
        cJSON *text = cJSON_GetObjectItem(block, "text");
        if (!text || !cJSON_IsString(text)) continue;
        size_t tlen = strlen(text->valuestring);
        size_t copy = (tlen < size - off - 1) ? tlen : size - off - 1;
        memcpy(buf + off, text->valuestring, copy);
        off += copy;
    }
    buf[off] = '\0';
}

static void extract_text_openai(cJSON *root, char *buf, size_t size)
{
    if (!buf || size < 2) return;
    buf[0] = '\0';
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices)) return;
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    if (!choice0) return;
    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    if (!message) return;
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!content || !cJSON_IsString(content)) return;

    size_t len = strlen(content->valuestring);
    if (len > size - 1) {
        len = size - 1;
        /* Back off to a character boundary.  Without this the copy ends on a
         * leading or continuation byte of a multi-byte character, and that
         * fragment is then valid UTF-8 as far as every later check is
         * concerned — it goes over BLE and into the transcript, where it shows
         * up as a replacement glyph in the middle of the next turn's context.
         *
         * Cutting the line short is still wrong for dialogue; this only stops it
         * also being invalid.  chat_fit_reply() in buddy_chat.c is what keeps the
         * cut at a sentence or clause break. */
        while (len > 0 && ((unsigned char)content->valuestring[len] & 0xC0) == 0x80) {
            len--;
        }
        ESP_LOGW(TAG, "LLM response was %u bytes and did not fit %u; "
                      "cut to %u at a character boundary",
                 (unsigned)strlen(content->valuestring), (unsigned)size,
                 (unsigned)len);
    }

    memcpy(buf, content->valuestring, len);
    buf[len] = '\0';
}

static cJSON *convert_tools_openai(const char *tools_json)
{
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
        }

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }

        if (!content || !cJSON_IsArray(content)) continue;

        if (strcmp(role->valuestring, "assistant") == 0) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");

            /* collect text */
            char *text_buf = NULL;
            size_t off = 0;
            cJSON *block;
            cJSON *tool_calls = NULL;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                    }
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0) {
                    if (!tool_calls) tool_calls = cJSON_CreateArray();
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (!name || !cJSON_IsString(name)) continue;

                    cJSON *tc = cJSON_CreateObject();
                    if (id && cJSON_IsString(id)) {
                        cJSON_AddStringToObject(tc, "id", id->valuestring);
                    }
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", name->valuestring);
                    if (input) {
                        char *args = cJSON_PrintUnformatted(input);
                        if (args) {
                            cJSON_AddStringToObject(func, "arguments", args);
                            free(args);
                        }
                    }
                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }
            if (text_buf) {
                cJSON_AddStringToObject(m, "content", text_buf);
            } else {
                cJSON_AddStringToObject(m, "content", "");
            }
            if (tool_calls) {
                cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            }
            cJSON_AddItemToArray(out, m);
            free(text_buf);
        } else if (strcmp(role->valuestring, "user") == 0) {
            /* tool_result blocks become role=tool */
            cJSON *block;
            bool has_user_text = false;
            char *text_buf = NULL;
            size_t off = 0;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if (!tool_id || !cJSON_IsString(tool_id)) continue;
                    cJSON *tm = cJSON_CreateObject();
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    } else if (tcontent && cJSON_IsArray(tcontent)) {
                        bool has_image = false;
                        cJSON *item;
                        cJSON_ArrayForEach(item, tcontent) {
                            cJSON *itype = cJSON_GetObjectItem(item, "type");
                            if (itype && cJSON_IsString(itype) &&
                                (strcmp(itype->valuestring, "image_url") == 0 ||
                                 strcmp(itype->valuestring, "image") == 0 ||
                                 strcmp(itype->valuestring, "input_image") == 0)) {
                                has_image = true;
                                break;
                            }
                        }

                        if (has_image) {
                            /* Keep a plain-string role=tool message for strict function-call protocol,
                             * and forward the real multimodal payload as an extra role=user message. */
                            cJSON_AddStringToObject(tm, "content", "[image tool result]");

                            cJSON *um = cJSON_CreateObject();
                            cJSON_AddStringToObject(um, "role", "user");
                            cJSON_AddItemToObject(um, "content", cJSON_Duplicate(tcontent, 1));
                            cJSON_AddItemToArray(out, um);
                        } else {
                            cJSON_AddItemToObject(tm, "content", cJSON_Duplicate(tcontent, 1));
                        }
                    } else {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                        has_user_text = true;
                    }
                }
            }
            if (has_user_text) {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddStringToObject(um, "content", text_buf);
                cJSON_AddItemToArray(out, um);
            }
            free(text_buf);
        }
    }

    return out;
}

/* ── Public: simple chat (backward compat) ────────────────────── */

/* Drop bytes that cannot be part of valid UTF-8.
 *
 * ESP-IDF's cJSON copies strings byte for byte and never validates them, so a
 * buffer that was truncated mid-character — a transcript hitting its byte limit,
 * a BLE frame cut at a byte boundary, a profile field from a web form — reaches
 * the provider as a malformed sequence.  The API rejects the entire request:
 *
 *     Failed to parse the request body as JSON: messages[0].content:
 *     invalid unicode code point at line 1 column 1978
 *
 * The column points into a request the badge cannot even print faithfully, so
 * hunting the originating buffer is a poor use of time.  This is the one place
 * every request passes through, so the malformed bytes are removed here and the
 * rest of the text still gets through. */
static void utf8_sanitize_inplace(char *s)
{
    if (!s) return;

    unsigned char *buf = (unsigned char *)s;
    size_t in  = 0;   /* read position  */
    size_t out = 0;   /* write position; never ahead of `in`, hence safe in place */

    while (buf[in] != '\0') {
        unsigned char c = buf[in];
        size_t need;

        if (c < 0x80) {
            buf[out++] = buf[in++];
            continue;
        }

        if ((c & 0xE0) == 0xC0)      need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else { in++; continue; }     /* stray continuation byte */

        size_t have = 1;
        while (have < need && buf[in + have] != '\0' &&
               (buf[in + have] & 0xC0) == 0x80) {
            have++;
        }

        if (have == need) {
            for (size_t k = 0; k < need; k++) {
                buf[out++] = buf[in + k];
            }
            in += need;
        } else {
            /* Truncated sequence — normally the tail of a buffer that ran out of
             * room.  Drop the lead byte and whatever continuation bytes were
             * present; the text already copied stays where it is. */
            in += have;
        }
    }

    s[out] = '\0';
}

/* Same treatment for every string inside a parsed messages array: the content
 * of a message is where free text ends up, and it may have been copied from a
 * buffer that was already cut mid-character. */
static void utf8_sanitize_json(cJSON *node)
{
    if (!node) return;

    if (cJSON_IsString(node) && node->valuestring) {
        utf8_sanitize_inplace(node->valuestring);
        return;
    }

    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        utf8_sanitize_json(child);
    }
}

/* Defined below llm_chat(), which calls it twice — once, and again when the
 * provider answers with an empty completion. */
static esp_err_t llm_chat_once(const char *post_data, const char *system_prompt,
                               char *response_buf, size_t buf_size,
                               bool *empty_out);

esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size)
{
    if (s_api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No API key configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build request body (non-streaming) */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    if (s_llm_provider == LLM_PROVIDER_OPENAI || s_llm_provider == LLM_PROVIDER_OPENROUTER) {
        cJSON_AddNumberToObject(body, "max_completion_tokens", MIMI_LLM_MAX_TOKENS);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    }

    /* This one goes into the body as well (as the fallback content when the
     * history will not parse), so it gets the same treatment. */
    char *messages_copy = messages_json ? strdup(messages_json) : NULL;

    if (s_llm_provider != LLM_PROVIDER_ANTHROPIC) {
        cJSON *messages = messages_copy ? cJSON_Parse(messages_copy) : NULL;
        if (!messages) {
            messages = cJSON_CreateArray();
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddItemToObject(msg, "content",
                                  cJSON_CreateString(messages_copy ? messages_copy : ""));
            cJSON_AddItemToArray(messages, msg);
        }
        utf8_sanitize_json(messages);
        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_Delete(messages);
        utf8_sanitize_json(openai_msgs);
        cJSON_AddItemToObject(body, "messages", openai_msgs);
    } else {
        /* The system prompt is caller-owned (often a static buffer), so it gets a
         * sanitized copy rather than being edited in place. */
        char *sys_copy = system_prompt ? strdup(system_prompt) : NULL;
        if (sys_copy) utf8_sanitize_inplace(sys_copy);
        cJSON_AddStringToObject(body, "system", sys_copy ? sys_copy : "");
        free(sys_copy);

        cJSON *messages = messages_copy ? cJSON_Parse(messages_copy) : NULL;
        if (messages) {
            utf8_sanitize_json(messages);
            cJSON_AddItemToObject(body, "messages", messages);
        } else {
            cJSON *arr = cJSON_CreateArray();
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddItemToObject(msg, "content",
                                  cJSON_CreateString(messages_copy ? messages_copy : ""));
            cJSON_AddItemToArray(arr, msg);
            cJSON_AddItemToObject(body, "messages", arr);
        }
    }

    free(messages_copy);

    /* cJSON never validates UTF-8: print_string_ptr() escapes control characters
     * and copies every other byte verbatim, so one truncated multi-byte
     * character anywhere in this tree reaches the provider as a malformed
     * sequence and the whole request is rejected.  This is the single point every
     * request passes through, so it is the one place worth guarding — it covers
     * the parts assembled above as well as anything nested inside them. */
    utf8_sanitize_json(body);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        snprintf(response_buf, buf_size, "Error: Failed to build request");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Calling LLM API (provider: %s, model: %s, body: %d bytes)",
             s_provider, s_model, (int)strlen(post_data));
    llm_log_payload("LLM request", post_data);

    /* Ask up to twice when the provider answers with nothing.
     *
     * deepseek-flash is a reasoning model: it can spend the whole completion on
     * its reasoning and return `content: ""` with `finish_reason: "stop"` — not
     * "length", because it considers itself finished. Measured on hardware, one
     * turn of seven did that, and the turn was lost to a placeholder line.
     *
     * A second identical request usually produces text, and the cost is one more
     * round-trip on a turn that would otherwise be wasted. Deliberately only one
     * retry: the session's think budget is what decides whether a reply is still
     * wanted, and a third attempt would spend it on a provider that is not going
     * to answer.
     *
     * Only an empty completion is retried. A transport error or a non-200 is
     * returned as it is — those have their own handling, and repeating a request
     * the provider already rejected does not help. */
    const int attempts = 2;
    esp_err_t err = ESP_FAIL;
    bool empty = false;

    for (int attempt = 1; attempt <= attempts; attempt++) {
        err = llm_chat_once(post_data, system_prompt, response_buf, buf_size, &empty);
        if (err != ESP_OK || !empty) break;

        if (attempt < attempts) {
            ESP_LOGW(TAG, "Empty completion — asking again (%d of %d)",
                     attempt + 1, attempts);
        } else {
            /* Out of attempts with nothing to show. The buffer is left empty —
             * that is the signal the caller's fallback path keys on — and the
             * text every other failure uses is written instead, so the log says
             * what happened rather than showing a blank line. */
            ESP_LOGW(TAG, "Empty completion on every attempt");
            snprintf(response_buf, buf_size, "No response from LLM API");
        }
    }

    free(post_data);
    return err;
}

/* One round-trip: post `post_data`, parse the reply, extract the text.
 *
 * Split out of llm_chat() so a turn whose completion came back empty can be
 * asked for again. `empty_out` distinguishes "the provider answered with
 * nothing" (worth retrying) from a transport or HTTP failure (not).
 */
static esp_err_t llm_chat_once(const char *post_data, const char *system_prompt,
                               char *response_buf, size_t buf_size,
                               bool *empty_out)
{
    *empty_out = false;

    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
        snprintf(response_buf, buf_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = llm_http_call(post_data, &rb, &status);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        llm_log_payload("LLM partial response", rb.data);
        resp_buf_free(&rb);
        snprintf(response_buf, buf_size, "Error: HTTP request failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    llm_log_payload("LLM raw response", rb.data);

    if (status != 200) {
        ESP_LOGE(TAG, "API returned status %d", status);
        snprintf(response_buf, buf_size, "API error (HTTP %d): %.200s",
                 status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(response_buf, buf_size, "Error: Failed to parse response");
        return ESP_FAIL;
    }

    if (s_llm_provider != LLM_PROVIDER_ANTHROPIC) {
        extract_text_openai(root, response_buf, buf_size);
    } else {
        extract_text_anthropic(root, response_buf, buf_size);
    }
    cJSON_Delete(root);

    /* Nothing extracted. Report it as its own outcome rather than as an error,
     * because the caller may want to try again — see the retry in llm_chat().
     *
     * This branch used to be unreachable in a way that mattered: it tested
     * `response_buf[0] == '\0'` immediately after writing the literal
     * "No response from LLM API" into that same buffer, so the buffer could not
     * be empty by then and the message "No response from LLM API" — which
     * callers match on to detect failure — was never the thing that told them.
     * Extraction failing and a real HTTP failure were told apart only by
     * accident, through extract_text_openai() having reset the buffer to "". */
    if (response_buf[0] == '\0') {
        *empty_out = true;
        ESP_LOGW(TAG, "Provider answered with an empty completion");
        return ESP_OK;      /* transport was fine; the content was not */
    }

    ESP_LOGI(TAG, "LLM response: %d bytes", (int)strlen(response_buf));
    return ESP_OK;
}

/* ── Public: chat with tools (non-streaming) ──────────────────── */

void llm_response_free(llm_response_t *resp)
{
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = NULL;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (s_api_key[0] == '\0') return ESP_ERR_INVALID_STATE;

    /* Build request body (non-streaming) */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    if (s_llm_provider == LLM_PROVIDER_OPENAI || s_llm_provider == LLM_PROVIDER_OPENROUTER) {
        cJSON_AddNumberToObject(body, "max_completion_tokens", MIMI_LLM_MAX_TOKENS);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    }

    if (s_llm_provider != LLM_PROVIDER_ANTHROPIC) {
        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_AddItemToObject(body, "messages", openai_msgs);

        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                cJSON_AddStringToObject(body, "tool_choice", "auto");
            }
        }
    } else {
        cJSON_AddStringToObject(body, "system", system_prompt);

        /* Deep-copy messages so caller keeps ownership */
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        cJSON_AddItemToObject(body, "messages", msgs_copy);

        /* Add tools array if provided */
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
            }
        }
    }

    /* Same guard as llm_chat(): tool results are arbitrary text from tools, and
     * any truncated multi-byte character in them would fail the whole request. */
    utf8_sanitize_json(body);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calling LLM API with tools (provider: %s, model: %s, body: %d bytes)",
             s_provider, s_model, (int)strlen(post_data));
    llm_log_payload("LLM tools request", post_data);

    /* HTTP call */
    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = llm_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        llm_log_payload("LLM tools partial response", rb.data);
        resp_buf_free(&rb);
        return err;
    }

    llm_log_payload("LLM tools raw response", rb.data);

    if (status != 200) {
        ESP_LOGE(TAG, "API error %d: %.500s", status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse full JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse API response JSON");
        return ESP_FAIL;
    }

    if (s_llm_provider != LLM_PROVIDER_ANTHROPIC) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        if (choice0) {
            cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
            if (finish && cJSON_IsString(finish)) {
                resp->tool_use = (strcmp(finish->valuestring, "tool_calls") == 0);
            }

            cJSON *message = cJSON_GetObjectItem(choice0, "message");
            if (message) {
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && cJSON_IsString(content)) {
                    size_t tlen = strlen(content->valuestring);
                    resp->text = calloc(1, tlen + 1);
                    if (resp->text) {
                        memcpy(resp->text, content->valuestring, tlen);
                        resp->text_len = tlen;
                    }
                }

                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *tc;
                    cJSON_ArrayForEach(tc, tool_calls) {
                        if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;
                        llm_tool_call_t *call = &resp->calls[resp->call_count];
                        cJSON *id = cJSON_GetObjectItem(tc, "id");
                        cJSON *func = cJSON_GetObjectItem(tc, "function");
                        if (id && cJSON_IsString(id)) {
                            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                        }
                        if (func) {
                            cJSON *name = cJSON_GetObjectItem(func, "name");
                            cJSON *args = cJSON_GetObjectItem(func, "arguments");
                            if (name && cJSON_IsString(name)) {
                                strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                            }
                            if (args && cJSON_IsString(args)) {
                                call->input = strdup(args->valuestring);
                                if (call->input) {
                                    call->input_len = strlen(call->input);
                                }
                            }
                        }
                        resp->call_count++;
                    }
                    if (resp->call_count > 0) {
                        resp->tool_use = true;
                    }
                }
            }
        }
    } else {
        /* stop_reason */
        cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
        if (stop_reason && cJSON_IsString(stop_reason)) {
            resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);
        }

        /* Iterate content blocks */
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (content && cJSON_IsArray(content)) {
            /* Accumulate total text length first */
            size_t total_text = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        total_text += strlen(text->valuestring);
                    }
                }
            }

            /* Allocate and copy text */
            if (total_text > 0) {
                resp->text = calloc(1, total_text + 1);
                if (resp->text) {
                    cJSON_ArrayForEach(block, content) {
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
                        cJSON *text = cJSON_GetObjectItem(block, "text");
                        if (!text || !cJSON_IsString(text)) continue;
                        size_t tlen = strlen(text->valuestring);
                        memcpy(resp->text + resp->text_len, text->valuestring, tlen);
                        resp->text_len += tlen;
                    }
                    resp->text[resp->text_len] = '\0';
                }
            }

            /* Extract tool_use blocks */
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || strcmp(btype->valuestring, "tool_use") != 0) continue;
                if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;

                llm_tool_call_t *call = &resp->calls[resp->call_count];

                cJSON *id = cJSON_GetObjectItem(block, "id");
                if (id && cJSON_IsString(id)) {
                    strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                }

                cJSON *name = cJSON_GetObjectItem(block, "name");
                if (name && cJSON_IsString(name)) {
                    strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                }

                cJSON *input = cJSON_GetObjectItem(block, "input");
                if (input) {
                    char *input_str = cJSON_PrintUnformatted(input);
                    if (input_str) {
                        call->input = input_str;
                        call->input_len = strlen(input_str);
                    }
                }

                resp->call_count++;
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
             (int)resp->text_len, resp->call_count,
             resp->tool_use ? "tool_use" : "end_turn");

    return ESP_OK;
}

/* ── NVS helpers ──────────────────────────────────────────────── */

esp_err_t llm_set_api_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    ESP_LOGI(TAG, "API key saved");
    return ESP_OK;
}

esp_err_t llm_set_model(const char *model)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_model, sizeof(s_model), model);
    ESP_LOGI(TAG, "Model set to: %s", s_model);
    return ESP_OK;
}

esp_err_t llm_set_provider(const char *provider)
{
    if (!provider || provider_parse(provider) == LLM_PROVIDER_UNKNOWN) {
        ESP_LOGE(TAG, "Unknown provider '%s'; valid: anthropic, openai, openrouter, nvidia, qwen, deepseek",
                 provider ? provider : "<null>");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_PROVIDER, provider));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_provider, sizeof(s_provider), provider);
    s_llm_provider = provider_parse(s_provider);
    ESP_LOGI(TAG, "Provider set to: %s", s_provider);
    return ESP_OK;
}
