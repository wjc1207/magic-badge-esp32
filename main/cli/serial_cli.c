#include "serial_cli.h"
#include "mimi_config.h"
#include "onboard/wifi_onboard.h"
#include "wifi/wifi_manager.h"
#include "channels/telegram/telegram_bot.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"
#include "buddy/buddy.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IPv4: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

/* --- tg_send command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_str *text;
    struct arg_end *end;
} tg_send_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    telegram_set_token(tg_token_args.token->sval[0]);
    printf("Telegram bot token saved.\n");
    return 0;
}

static int cmd_tg_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_send_args.end, argv[0]);
        return 1;
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, tg_send_args.chat_id->sval[0], sizeof(msg.chat_id) - 1);
    strncpy(msg.type, "text", sizeof(msg.type) - 1);
    msg.payload.text = (char *)tg_send_args.text->sval[0];
    esp_err_t err = message_bus_push_outbound(&msg);
    printf("tg_send status: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_feishu_creds command --- */
static struct {
    struct arg_str *app_id;
    struct arg_str *app_secret;
    struct arg_end *end;
} feishu_creds_args;

/* --- feishu_send command --- */
static struct {
    struct arg_str *receive_id;
    struct arg_str *text;
    struct arg_end *end;
} feishu_send_args;

static int cmd_set_feishu_creds(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_creds_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_creds_args.end, argv[0]);
        return 1;
    }
    feishu_set_credentials(feishu_creds_args.app_id->sval[0],
                           feishu_creds_args.app_secret->sval[0]);
    printf("Feishu credentials saved.\n");
    return 0;
}

static int cmd_feishu_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_send_args.end, argv[0]);
        return 1;
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, feishu_send_args.receive_id->sval[0], sizeof(msg.chat_id) - 1);
    strncpy(msg.type, "text", sizeof(msg.type) - 1);
    msg.payload.text = (char *)feishu_send_args.text->sval[0];
    esp_err_t err = message_bus_push_outbound(&msg);
    printf("feishu_send status: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- set_search_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} search_provider_args;

static int cmd_set_search_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_provider_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = tool_web_search_set_provider(search_provider_args.provider->sval[0]);
    if (err == ESP_OK) {
        printf("Search provider set.\n");
    } else {
        printf("Invalid provider. Use 'tavily' or 'brave'.\n");
    }
    return (err == ESP_OK) ? 0 : 1;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- skill_list command --- */
static int cmd_skill_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }

    size_t n = skill_loader_build_summary(buf, 4096);
    if (n == 0) {
        printf("No skills found under /spiffs/skills/.\n");
    } else {
        printf("=== Skills ===\n%s", buf);
    }
    free(buf);
    return 0;
}

/* --- skill_show command --- */
static struct {
    struct arg_str *name;
    struct arg_end *end;
} skill_show_args;

static bool has_md_suffix(const char *name)
{
    size_t len = strlen(name);
    return (len >= 3) && strcmp(name + len - 3, ".md") == 0;
}

static bool build_skill_path(const char *name, char *out, size_t out_size)
{
    if (!name || !name[0]) return false;
    if (strstr(name, "..") != NULL) return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;

    if (has_md_suffix(name)) {
        snprintf(out, out_size, "/spiffs/skills/%s", name);
    } else {
        snprintf(out, out_size, "/spiffs/skills/%s.md", name);
    }
    return true;
}

static int cmd_skill_show(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_show_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_show_args.end, argv[0]);
        return 1;
    }

    char path[128];
    if (!build_skill_path(skill_show_args.name->sval[0], path, sizeof(path))) {
        printf("Invalid skill name.\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Skill not found: %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stdout);
    }
    fclose(f);
    printf("\n============\n");
    return 0;
}

/* --- skill_search command --- */
static struct {
    struct arg_str *keyword;
    struct arg_end *end;
} skill_search_args;

static bool contains_nocase(const char *text, const char *keyword)
{
    if (!text || !keyword || !keyword[0]) return false;

    size_t key_len = strlen(keyword);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < key_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)keyword[i])) {
            i++;
        }
        if (i == key_len) return true;
    }
    return false;
}

static int cmd_skill_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_search_args.end, argv[0]);
        return 1;
    }

    const char *keyword = skill_search_args.keyword->sval[0];
    DIR *dir = opendir("/spiffs");
    if (!dir) {
        printf("Cannot open /spiffs.\n");
        return 1;
    }

    const char *prefix = "skills/";
    const size_t prefix_len = strlen(prefix);
    int matches = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (strncmp(name, prefix, prefix_len) != 0) continue;
        if (name_len < prefix_len + 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[296];
        snprintf(full_path, sizeof(full_path), "/spiffs/%s", name);

        bool file_matched = contains_nocase(name, keyword);
        int matched_line = 0;

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int line_no = 0;
        while (!file_matched && fgets(line, sizeof(line), f)) {
            line_no++;
            if (contains_nocase(line, keyword)) {
                file_matched = true;
                matched_line = line_no;
            }
        }
        fclose(f);

        if (file_matched) {
            matches++;
            if (matched_line > 0) {
                printf("- %s (matched at line %d)\n", full_path, matched_line);
            } else {
                printf("- %s (matched in filename)\n", full_path);
            }
        }
    }

    closedir(dir);
    if (matches == 0) {
        printf("No skills matched keyword: %s\n", keyword);
    } else {
        printf("Total matches: %d\n", matches);
    }
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static void print_config_int(const char *label, const char *ns, const char *nvs_key, const char *default_val)
{
    char value[32];
    strncpy(value, default_val, sizeof(value) - 1);
    const char *source = "build";

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        int32_t temp;
        if (nvs_get_i32(nvs, nvs_key, &temp) == ESP_OK) {
            snprintf(value, sizeof(value), "%d", (int)temp);
            source = "NVS";
        }
        nvs_close(nvs);
    }

    if (strcmp(source, "not set") == 0) {
        source = "build";
    }

    printf("  %-14s: %-10s  [%s]\n", label, value, source);
}

static void print_config_bool(const char *label, const char *ns, const char *key,
                              bool build_val)
{
    const char *source = "not set";
    bool value = build_val;

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t bool_val = 0;
        if (nvs_get_u8(nvs, key, &bool_val) == ESP_OK) {
            source = "NVS";
            value = bool_val ? true : false;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0) {
        source = "build";
    }

    printf("  %-14s: %-5s  [%s]\n", label, value ? "true" : "false", source);
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");

    /* WiFi */
    print_config("WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);

    /* Telegram */
    print_config("TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);

    /* Feishu */
    print_config("FS App ID",  MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, MIMI_SECRET_FEISHU_APP_ID, true);
    print_config("FS Secret",  MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET, MIMI_SECRET_FEISHU_APP_SECRET, true);

    /* LLM */
    print_config("API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    print_config("Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    print_config("Provider",   MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER, false);
    print_config("Sys Prompt", MIMI_NVS_LLM,    MIMI_NVS_KEY_SYSTEM_PROMPT, "", false);

    /* Search */
    print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);
    print_config("Tavily Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY, MIMI_SECRET_TAVILY_KEY, true);
    print_config("Search Prov", MIMI_NVS_SEARCH, MIMI_NVS_KEY_SEARCH_PROVIDER, MIMI_SECRET_SEARCH_PROVIDER, false);

    /* Proxy */
    print_config("Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    print_config("Proxy Port", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT, false);
    print_config("Proxy Type", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_TYPE, MIMI_SECRET_PROXY_TYPE, false);

    /* Camera Configuration */
    printf("\n=== Camera Configuration ===\n");
    print_config_int("Cam Frame Size", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAMERA_FRAME_SIZE, "0");
    print_config_int("Cam JPEG Quality", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAMERA_JPEG_QUALITY, "8");
    print_config_int("Cam PWDN Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_PWDN, "-1");
    print_config_int("Cam Reset Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_RESET, "-1");
    print_config_int("Cam XCLK Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_XCLK, "-1");
    print_config_int("Cam SIOD Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_SIOD, "-1");
    print_config_int("Cam SIOC Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_SIOC, "-1");
    print_config_int("Cam D7 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D7, "-1");
    print_config_int("Cam D6 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D6, "-1");
    print_config_int("Cam D5 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D5, "-1");
    print_config_int("Cam D4 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D4, "-1");
    print_config_int("Cam D3 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D3, "-1");
    print_config_int("Cam D2 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D2, "-1");
    print_config_int("Cam D1 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D1, "-1");
    print_config_int("Cam D0 Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_D0, "-1");
    print_config_int("Cam VSYNC Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_VSYNC, "-1");
    print_config_int("Cam HREF Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_HREF, "-1");
    print_config_int("Cam PCLK Pin", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_PIN_PCLK, "-1");
    print_config_int("Cam XCLK Freq", MIMI_NVS_FEATURE, MIMI_NVS_KEY_CAM_XCLK_FREQ, "12000000");

    /* BLE Configuration */
    printf("\n=== BLE Configuration ===\n");
    print_config("BLE Target Addr", MIMI_NVS_FEATURE, MIMI_NVS_KEY_BLE_TARGET_ADDR, MIMI_BLE_TARGET_ADDR, false);
    
    /* Features */
    printf("\n=== Features ===\n");
    print_config_bool("Telegram Bot",  MIMI_NVS_FEATURE, MIMI_NVS_KEY_TELEGRAM_BOT,    MIMI_FEATURE_TELEGRAM_BOT);
    print_config_bool("Feishu Bot",    MIMI_NVS_FEATURE, MIMI_NVS_KEY_FEISHU_BOT,      MIMI_FEATURE_FEISHU_BOT);

    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_FEISHU, MIMI_NVS_LLM, MIMI_NVS_PROXY, MIMI_NVS_SEARCH, MIMI_NVS_FEATURE
    };
    for (int i = 0; i < 7; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- heartbeat_trigger command --- */
static int cmd_heartbeat_trigger(int argc, char **argv)
{
    printf("Checking HEARTBEAT.md...\n");
    if (heartbeat_trigger()) {
        printf("Heartbeat: agent prompted with pending tasks.\n");
    } else {
        printf("Heartbeat: no actionable tasks found.\n");
    }
    return 0;
}

/* --- cron_start command --- */
static int cmd_cron_start(int argc, char **argv)
{
    esp_err_t err = cron_service_start();
    if (err == ESP_OK) {
        printf("Cron service started.\n");
        return 0;
    }

    printf("Failed to start cron service: %s\n", esp_err_to_name(err));
    return 1;
}

static int cmd_tool_exec(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tool_exec <name> [json]\n");
        return 1;
    }

    const char *tool_name = argv[1];
    const char *input_json = (argc >= 3) ? argv[2] : "{}";

    char *output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json, output, 4096);
    printf("tool_exec status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_feature command --- */
static struct {
    struct arg_str *feature;
    struct arg_str *value;
    struct arg_end *end;
} set_feature_args;

static int cmd_set_feature(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&set_feature_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, set_feature_args.end, argv[0]);
        return 1;
    }

    const char *feature = set_feature_args.feature->sval[0];
    const char *value_str = set_feature_args.value->sval[0];
    bool value = (strcmp(value_str, "true") == 0 || strcmp(value_str, "1") == 0);

    esp_err_t err = ESP_OK;

    if (strcmp(feature, "telegram_bot") == 0) {
        err = mimi_set_feature_telegram_bot(value);
    } else if (strcmp(feature, "feishu_bot") == 0) {
        err = mimi_set_feature_feishu_bot(value);
    } else {
        printf("Unknown feature: %s. Use: telegram_bot, feishu_bot\n", feature);
        return 1;
    }

    if (err == ESP_OK) {
        printf("Feature %s set to %s. Restart to apply.\n", feature, value ? "true" : "false");
    } else {
        printf("Failed to set feature %s: %s\n", feature, esp_err_to_name(err));
    }

    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_ble_target_addr command --- */
static struct {
    struct arg_str *addr;
    struct arg_end *end;
} ble_addr_args;

static int cmd_set_ble_target_addr(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ble_addr_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ble_addr_args.end, argv[0]);
        return 1;
    }

    const char *addr = ble_addr_args.addr->sval[0];
    esp_err_t err = mimi_set_ble_target_addr(addr);

    if (err == ESP_OK) {
        printf("BLE target address set to %s. Restart to apply.\n", addr);
    } else {
        printf("Failed to set BLE target address: %s\n", esp_err_to_name(err));
    }

    return (err == ESP_OK) ? 0 : 1;
}

/* --- buddy_status command --- */
static int cmd_buddy_status(int argc, char **argv)
{
    const buddy_identity_t *id = buddy_identity_get();
    printf("=== Buddy System Status ===\n");
    printf("Device ID:    %s\n", id->device_id);

    buddy_profile_t *profile = heap_caps_calloc(1, sizeof(*profile), MALLOC_CAP_SPIRAM);
    if (profile) {
        if (buddy_profile_get(profile) == ESP_OK) {
            printf("\n--- Profile ---\n");
            printf("Name:   %s\n", profile->display_name);
            printf("Bio:    %s\n", profile->bio);
            printf("Look:   %s\n", profile->appearance[0] ? profile->appearance : "(not set)");
            printf("Carry:  %s\n", profile->belongings[0] ? profile->belongings : "(not set)");
            printf("Traits: %s\n", profile->traits[0] ? profile->traits : "(not set)");
            printf("Tech:   %s\n", profile->tech_level[0] ? profile->tech_level : "(not set)");
            printf("Hash:   %02x%02x%02x%02x%02x%02x%02x%02x\n",
                   profile->profile_hash[0], profile->profile_hash[1],
                   profile->profile_hash[2], profile->profile_hash[3],
                   profile->profile_hash[4], profile->profile_hash[5],
                   profile->profile_hash[6], profile->profile_hash[7]);
        }
        heap_caps_free(profile);
    }

    printf("Privacy: %s\n",
           buddy_privacy_get() == BUDDY_MODE_PUBLIC ? "PUBLIC" : "PRIVATE");

    buddy_contact_record_t *recs = heap_caps_calloc(10, sizeof(*recs), MALLOC_CAP_SPIRAM);
    if (recs) {
        size_t count = 0;
        buddy_contacts_list(recs, 10, &count);
        printf("\nContacts: %d\n", (int)count);
        for (size_t i = 0; i < count && i < 5; i++) {
            printf("  %s - %s (met %lld, x%d)\n",
                   recs[i].peer_id, recs[i].display_name,
                   (long long)recs[i].last_met_unix,
                   (int)recs[i].meeting_count);
        }
        if (count > 5) printf("  ...and %d more\n", (int)(count - 5));
        heap_caps_free(recs);
    }
    printf("===========================\n");
    return 0;
}

/* --- buddy_privacy command --- */
static struct {
    struct arg_str *mode;
    struct arg_end *end;
} buddy_privacy_args;

static int cmd_buddy_privacy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&buddy_privacy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, buddy_privacy_args.end, argv[0]);
        return 1;
    }

    const char *mode = buddy_privacy_args.mode->sval[0];
    if (strcmp(mode, "public") == 0) {
        buddy_privacy_set(BUDDY_MODE_PUBLIC);
        printf("Buddy privacy: PUBLIC (broadcasting)\n");
    } else if (strcmp(mode, "private") == 0) {
        buddy_privacy_set(BUDDY_MODE_PRIVATE);
        printf("Buddy privacy: PRIVATE (silent)\n");
    } else {
        printf("Invalid mode. Use 'public' or 'private'.\n");
        return 1;
    }
    return 0;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mimi> ";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    /* Register commands */
    esp_console_register_help_command();

    /* set_wifi */
    wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "set_wifi",
        .help = "Set WiFi SSID and password (e.g. set_wifi MySSID MyPass)",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan and list nearby WiFi APs",
        .func = &cmd_wifi_scan,
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command = "set_tg_token",
        .help = "Set Telegram bot token",
        .func = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* tg_send */
    tg_send_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Telegram chat ID");
    tg_send_args.text = arg_str1(NULL, NULL, "<text>", "Text message (quote if contains spaces)");
    tg_send_args.end = arg_end(2);
    esp_console_cmd_t tg_send_cmd = {
        .command = "tg_send",
        .help = "Send Telegram text: tg_send <chat_id> \"hello\"",
        .func = &cmd_tg_send,
        .argtable = &tg_send_args,
    };
    esp_console_cmd_register(&tg_send_cmd);

    /* set_feishu_creds */
    feishu_creds_args.app_id = arg_str1(NULL, NULL, "<app_id>", "Feishu App ID");
    feishu_creds_args.app_secret = arg_str1(NULL, NULL, "<app_secret>", "Feishu App Secret");
    feishu_creds_args.end = arg_end(2);
    esp_console_cmd_t feishu_creds_cmd = {
        .command = "set_feishu_creds",
        .help = "Set Feishu app credentials (app_id app_secret)",
        .func = &cmd_set_feishu_creds,
        .argtable = &feishu_creds_args,
    };
    esp_console_cmd_register(&feishu_creds_cmd);

    /* feishu_send */
    feishu_send_args.receive_id = arg_str1(NULL, NULL, "<receive_id>", "Feishu open_id/chat_id");
    feishu_send_args.text = arg_str1(NULL, NULL, "<text>", "Text message (quote if contains spaces)");
    feishu_send_args.end = arg_end(2);
    esp_console_cmd_t feishu_send_cmd = {
        .command = "feishu_send",
        .help = "Send Feishu text: feishu_send <open_id|chat_id> \"hello\"",
        .func = &cmd_feishu_send,
        .argtable = &feishu_send_args,
    };
    esp_console_cmd_register(&feishu_send_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set LLM API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Model provider (anthropic|openai)");
    provider_args.end = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider",
        .help = "Set LLM model provider (default: " MIMI_LLM_PROVIDER_DEFAULT ")",
        .func = &cmd_set_model_provider,
        .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* skill_list */
    esp_console_cmd_t skill_list_cmd = {
        .command = "skill_list",
        .help = "List installed skills from /spiffs/skills/",
        .func = &cmd_skill_list,
    };
    esp_console_cmd_register(&skill_list_cmd);

    /* skill_show */
    skill_show_args.name = arg_str1(NULL, NULL, "<name>", "Skill name (e.g. weather or weather.md)");
    skill_show_args.end = arg_end(1);
    esp_console_cmd_t skill_show_cmd = {
        .command = "skill_show",
        .help = "Print full content of one skill file",
        .func = &cmd_skill_show,
        .argtable = &skill_show_args,
    };
    esp_console_cmd_register(&skill_show_cmd);

    /* skill_search */
    skill_search_args.keyword = arg_str1(NULL, NULL, "<keyword>", "Keyword to search in skills");
    skill_search_args.end = arg_end(1);
    esp_console_cmd_t skill_search_cmd = {
        .command = "skill_search",
        .help = "Search skill files by keyword (filename + content)",
        .func = &cmd_skill_search,
        .argtable = &skill_search_args,
    };
    esp_console_cmd_register(&skill_search_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Search API key (Tavily or Brave)");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set search API key for web_search tool (Tavily or Brave)",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_search_provider */
    search_provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Search provider (tavily|brave)");
    search_provider_args.end = arg_end(1);
    esp_console_cmd_t search_provider_cmd = {
        .command = "set_search_provider",
        .help = "Set web search provider (default: tavily)",
        .func = &cmd_set_search_provider,
        .argtable = &search_provider_args,
    };
    esp_console_cmd_register(&search_provider_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.end = arg_end(2);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set HTTP proxy (e.g. set_proxy 192.168.1.83 7897)",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* heartbeat_trigger */
    esp_console_cmd_t heartbeat_cmd = {
        .command = "heartbeat_trigger",
        .help = "Manually trigger a heartbeat check",
        .func = &cmd_heartbeat_trigger,
    };
    esp_console_cmd_register(&heartbeat_cmd);

    /* cron_start */
    esp_console_cmd_t cron_start_cmd = {
        .command = "cron_start",
        .help = "Start cron scheduler timer now",
        .func = &cmd_cron_start,
    };
    esp_console_cmd_register(&cron_start_cmd);

    /* tool_exec */
    esp_console_cmd_t tool_exec_cmd = {
        .command = "tool_exec",
        .help = "Execute a registered tool: tool_exec <name> {...json...}",
        .func = &cmd_tool_exec,
    };
    esp_console_cmd_register(&tool_exec_cmd);

    /* set_feature */
    set_feature_args.feature = arg_str1(NULL, NULL, "<feature>", "Feature name (telegram_bot|feishu_bot)");
    set_feature_args.value = arg_str1(NULL, NULL, "<value>", "Value (true|false|1|0)");
    set_feature_args.end = arg_end(2);
    esp_console_cmd_t set_feature_cmd = {
        .command = "set_feature",
        .help = "Enable/disable a feature: set_feature <name> <true|false>",
        .func = &cmd_set_feature,
        .argtable = &set_feature_args,
    };
    esp_console_cmd_register(&set_feature_cmd);

    /* set_ble_target_addr */
    ble_addr_args.addr = arg_str1(NULL, NULL, "<address>", "BLE target address (e.g. a4:c1:38:a0:0d:98)");
    ble_addr_args.end = arg_end(1);
    esp_console_cmd_t set_ble_addr_cmd = {
        .command = "set_ble_target_addr",
        .help = "Set BLE target address: set_ble_target_addr <mac>",
        .func = &cmd_set_ble_target_addr,
        .argtable = &ble_addr_args,
    };
    esp_console_cmd_register(&set_ble_addr_cmd);

    /* buddy_status */
    esp_console_cmd_t buddy_status_cmd = {
        .command = "buddy_status",
        .help = "Show buddy system status (identity, profile, contacts)",
        .func = &cmd_buddy_status,
    };
    esp_console_cmd_register(&buddy_status_cmd);

    /* buddy_privacy */
    buddy_privacy_args.mode = arg_str1(NULL, NULL, "<mode>", "public or private");
    buddy_privacy_args.end = arg_end(1);
    esp_console_cmd_t buddy_privacy_cmd = {
        .command = "buddy_privacy",
        .help = "Set buddy privacy mode (public|private)",
        .func = &cmd_buddy_privacy,
        .argtable = &buddy_privacy_args,
    };
    esp_console_cmd_register(&buddy_privacy_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* Start REPL */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
