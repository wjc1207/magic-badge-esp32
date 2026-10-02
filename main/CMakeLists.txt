set(srcs
    "mimi.c"
    "bus/message_bus.c"
    "wifi/wifi_manager.c"
    "channels/telegram/telegram_bot.c"
    "channels/feishu/feishu_bot.c"

    "llm/llm_proxy.c"
    "agent/agent_loop.c"
    "agent/context_builder.c"
    "memory/memory_store.c"
    "memory/session_mgr.c"
    "gateway/ws_server.c"
    "cli/serial_cli.c"
    "ota/ota_manager.c"
    "proxy/http_proxy.c"
    "cron/cron_service.c"
    "heartbeat/heartbeat.c"
    "tools/tool_registry.c"
    "tools/tool_cron.c"
    "tools/tool_web_search.c"
    "tools/tool_get_time.c"
    "tools/tool_files.c"
    "tools/tool_http_request.c"

    "tools/tool_script.c"
    "lua/lua_runner.c"
    "lua/lua_gpio_lib.c"
    "skills/skill_loader.c"
    "onboard/wifi_onboard.c"
    "buddy/buddy.c"
    "buddy/buddy_profile.c"
    "buddy/buddy_proximity.c"
    "buddy/buddy_contacts.c"
    "buddy/buddy_ble.c"
    "buddy/buddy_chat.c"
    "buddy/buddy_agent.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls
        driver esp_timer led_strip lua esp_websocket_client esp32-camera bt mbedtls
        camera ble rgb
)
