#pragma once

#include "sdkconfig.h"

/* Magic Badge Global Configuration */

/* Optional feature toggles */

#ifndef MIMI_BLE_TARGET_ADDR
#define MIMI_BLE_TARGET_ADDR "a4:c1:38:a0:0d:98"
#endif

#ifndef MIMI_FEATURE_TELEGRAM_BOT
#define MIMI_FEATURE_TELEGRAM_BOT 1
#endif

#ifndef MIMI_FEATURE_FEISHU_BOT
#define MIMI_FEATURE_FEISHU_BOT 1
#endif

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_ID
#define MIMI_SECRET_FEISHU_APP_ID   ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_SECRET
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#endif
#ifndef MIMI_SECRET_FEISHU_WEBHOOK_TLS_CERT_PEM
#define MIMI_SECRET_FEISHU_WEBHOOK_TLS_CERT_PEM ""
#endif
#ifndef MIMI_SECRET_FEISHU_WEBHOOK_TLS_KEY_PEM
#define MIMI_SECRET_FEISHU_WEBHOOK_TLS_KEY_PEM  ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_PROXY_TYPE
#define MIMI_SECRET_PROXY_TYPE      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif
#ifndef MIMI_SECRET_TAVILY_KEY
#define MIMI_SECRET_TAVILY_KEY      ""
#endif
#ifndef MIMI_SECRET_SEARCH_PROVIDER
#define MIMI_SECRET_SEARCH_PROVIDER "tavily"
#endif
#ifndef MIMI_ONBOARD_AP_PREFIX
#define MIMI_ONBOARD_AP_PREFIX    "MagicBadge-"
#endif
#ifndef MIMI_ONBOARD_AP_PASS
#define MIMI_ONBOARD_AP_PASS      "12345678"  /* WPA2 requires at least 8 chars */
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#define MIMI_TG_POLL_STACK           (12 * 1024)
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0
#define MIMI_TG_CARD_SHOW_MS         3000
#define MIMI_TG_CARD_BODY_SCALE      3

/* Feishu Bot */
#define MIMI_FEISHU_MAX_MSG_LEN      4096
#define MIMI_FEISHU_POLL_STACK       (12 * 1024)
#define MIMI_FEISHU_POLL_PRIO        5
#define MIMI_FEISHU_POLL_CORE        0
#define MIMI_FEISHU_WEBHOOK_PORT     18790
#define MIMI_FEISHU_WEBHOOK_PATH     "/feishu/events"
#define MIMI_FEISHU_WEBHOOK_MAX_BODY (16 * 1024)

/* Agent Loop */
#define MIMI_AGENT_STACK             (24 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     12
#define MIMI_MAX_TOOL_CALLS          4
#define MIMI_AGENT_SEND_WORKING_STATUS 1

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "CST-8"  /* China Standard Time (UTC+8) */

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define MIMI_LLM_PROVIDER_DEFAULT    "anthropic"
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define MIMI_OPENROUTER_API_URL      "https://openrouter.ai/api/v1/chat/completions"
#define MIMI_NVIDIA_API_URL          "https://integrate.api.nvidia.com/v1/chat/completions"
#define MIMI_QWEN_API_URL            "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define MIMI_DEEPSEEK_API_URL        "https://api.deepseek.com/v1/chat/completions"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)
#define MIMI_LLM_RESP_MAX_BYTES      (512 * 1024)
#define MIMI_LLM_LOG_VERBOSE_PAYLOAD 0
#define MIMI_LLM_LOG_PREVIEW_BYTES   640

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           16
#define MIMI_OUTBOUND_STACK          (12 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       MIMI_SPIFFS_BASE "/config"
#define MIMI_SPIFFS_MEMORY_DIR       MIMI_SPIFFS_BASE "/memory"
#define MIMI_SPIFFS_SESSION_DIR      MIMI_SPIFFS_BASE "/sessions"
#define MIMI_MEMORY_FILE             MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md"
#define MIMI_SOUL_FILE               MIMI_SPIFFS_CONFIG_DIR "/SOUL.md"
#define MIMI_USER_FILE               MIMI_SPIFFS_CONFIG_DIR "/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

/* Cron / Heartbeat */
#define MIMI_CRON_FILE               MIMI_SPIFFS_BASE "/cron.json"
#define MIMI_CRON_MAX_JOBS           16
#define MIMI_CRON_CHECK_INTERVAL_MS  (60 * 1000)
#define MIMI_HEARTBEAT_FILE          MIMI_SPIFFS_BASE "/HEARTBEAT.md"
#define MIMI_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* Skills */
#define MIMI_SKILLS_PREFIX           MIMI_SPIFFS_BASE "/skills/"

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#define MIMI_WS_MAX_CLIENTS          4

/* Camera Debug Server */
#define MIMI_CAMERA_SERVER_PORT      18787

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_FEISHU              "feishu_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"
#define MIMI_NVS_FEATURE             "feature_config"

/* NVS Keys for Features */
#define MIMI_NVS_KEY_BLE_TARGET_ADDR "ble_target_addr"
#define MIMI_NVS_KEY_TELEGRAM_BOT    "telegram_bot"
#define MIMI_NVS_KEY_FEISHU_BOT      "feishu_bot"
#define MIMI_NVS_KEY_LAST_SRC_CHANNEL "last_chan"
#define MIMI_NVS_KEY_LAST_SRC_CHAT_ID "last_chid"
#define MIMI_NVS_KEY_BUDDY_NOTIFY_CHANNEL "buddy_chan"
#define MIMI_NVS_KEY_BUDDY_NOTIFY_CHAT_ID "buddy_chid"
#define MIMI_NVS_KEY_CAMERA_FRAME_SIZE "cam_frame_size"
#define MIMI_NVS_KEY_CAMERA_JPEG_QUALITY "cam_jpeg_qual"
#define MIMI_NVS_KEY_CAM_PIN_PWDN    "cam_pin_pwdn"
#define MIMI_NVS_KEY_CAM_PIN_RESET   "cam_pin_reset"
#define MIMI_NVS_KEY_CAM_PIN_XCLK    "cam_pin_xclk"
#define MIMI_NVS_KEY_CAM_PIN_SIOD    "cam_pin_siod"
#define MIMI_NVS_KEY_CAM_PIN_SIOC    "cam_pin_sioc"
#define MIMI_NVS_KEY_CAM_PIN_D7      "cam_pin_d7"
#define MIMI_NVS_KEY_CAM_PIN_D6      "cam_pin_d6"
#define MIMI_NVS_KEY_CAM_PIN_D5      "cam_pin_d5"
#define MIMI_NVS_KEY_CAM_PIN_D4      "cam_pin_d4"
#define MIMI_NVS_KEY_CAM_PIN_D3      "cam_pin_d3"
#define MIMI_NVS_KEY_CAM_PIN_D2      "cam_pin_d2"
#define MIMI_NVS_KEY_CAM_PIN_D1      "cam_pin_d1"
#define MIMI_NVS_KEY_CAM_PIN_D0      "cam_pin_d0"
#define MIMI_NVS_KEY_CAM_PIN_VSYNC   "cam_pin_vsync"
#define MIMI_NVS_KEY_CAM_PIN_HREF    "cam_pin_href"
#define MIMI_NVS_KEY_CAM_PIN_PCLK    "cam_pin_pclk"
#define MIMI_NVS_KEY_CAM_XCLK_FREQ  "cam_xclk_freq"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_FEISHU_APP_ID   "app_id"
#define MIMI_NVS_KEY_FEISHU_APP_SECRET "app_secret"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_SYSTEM_PROMPT   "system_prompt"
#define MIMI_NVS_KEY_TAVILY_KEY      "tavily_key"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
#define MIMI_NVS_KEY_PROXY_TYPE      "proxy_type"
#define MIMI_NVS_KEY_SEARCH_PROVIDER "search_provider"

/* WiFi Onboarding (Captive Portal) */
#define MIMI_ONBOARD_HTTP_PORT    80
#define MIMI_ONBOARD_DNS_STACK    (4 * 1024)
#define MIMI_ONBOARD_MAX_SCAN     20

/* Buddy Social System */
#define MIMI_FEATURE_BUDDY_ENABLED  1
#define MIMI_BUDDY_CONTACT_STACK    (8 * 1024)
#define MIMI_BUDDY_CONTACT_PRIO     5
#define MIMI_BUDDY_CONTACT_CORE     1
#define MIMI_BUDDY_BEACON_STACK     (4 * 1024)
#define MIMI_BUDDY_BEACON_PRIO      4
#define MIMI_BUDDY_BEACON_CORE      0
#define MIMI_BUDDY_NVS_NS           "buddy"
