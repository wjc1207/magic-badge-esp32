#include "buddy_agent.h"
#include "buddy_ble.h"
#include "buddy_contacts.h"
#include "buddy_profile.h"
#include "buddy_proximity.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "led_strip.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static const char *TAG = "buddy_agent";

/* ── LED driver (WS2812B on GPIO48) ──────────────────────────── */
#define BUDDY_LED_GPIO           48
#define BUDDY_LED_RMT_RES_HZ     (10 * 1000 * 1000)

static led_strip_handle_t s_led_strip = NULL;

static esp_err_t led_init(void)
{
    if (s_led_strip) return ESP_OK;

    led_strip_config_t cfg = {
        .strip_gpio_num = BUDDY_LED_GPIO,
        .max_leds = 3,  /* 3 LEDs for proximity ring */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = BUDDY_LED_RMT_RES_HZ,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    return led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_led_strip);
}

static void led_set_rgb(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_strip) return;
    led_strip_set_pixel(s_led_strip, idx, r, g, b);
    led_strip_refresh(s_led_strip);
}

esp_err_t buddy_led_set(buddy_led_pattern_t pattern)
{
    led_init();

    switch (pattern) {
    case BUDDY_LED_PATTERN_OFF:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 0, 0);
        break;
    case BUDDY_LED_PATTERN_BLUE_SLOW:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 0, 32);
        break;
    case BUDDY_LED_PATTERN_AMBER:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 255, 64, 0);
        break;
    case BUDDY_LED_PATTERN_GREEN_FAST:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 255, 0);
        break;
    case BUDDY_LED_PATTERN_AMBER_BRIEF:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 255, 64, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 0, 0);
        break;
    case BUDDY_LED_PATTERN_CHAT:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 96, 0, 160);  /* violet */
        break;
    }
    return ESP_OK;
}

/* ── Contact processing task ──────────────────────────────────── */
static void buddy_contact_task(void *arg)
{
    QueueHandle_t evt_queue = buddy_ble_get_event_queue();
    if (!evt_queue) {
        ESP_LOGE(TAG, "No event queue, aborting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Buddy contact task started on Core %d", xPortGetCoreID());

    while (1) {
        buddy_event_t evt;
        if (xQueueReceive(evt_queue, &evt, portMAX_DELAY) != pdTRUE) continue;

        if (evt.type != BUDDY_EVT_PROFILE_READY || !evt.peer_profile_valid) {
            heap_caps_free(evt.peer_profile);
            continue;
        }

        const char *peer_id = evt.peer_device_id[0] ? evt.peer_device_id : "unknown";
        ESP_LOGI(TAG, "Processing contact: %s (rssi=%d, prox=%s)",
                 peer_id, evt.rssi, buddy_proximity_str(evt.proximity));

        /* Check contact status */
        buddy_contact_status_t cstat = buddy_contacts_check(peer_id);

        /* LED feedback */
        switch (cstat) {
        case BUDDY_CONTACT_NEW:
            buddy_led_set(BUDDY_LED_PATTERN_GREEN_FAST);
            break;
        case BUDDY_CONTACT_KNOWN:
            buddy_led_set(BUDDY_LED_PATTERN_BLUE_SLOW);
            break;
        case BUDDY_CONTACT_RECENT:
            /* Silent — met within 24h, don't spam */
            ESP_LOGI(TAG, "Recent contact, skipping");
            continue;
        }

        /* Store contact locally */
        buddy_contact_record_t *rec = heap_caps_calloc(1, sizeof(*rec), MALLOC_CAP_SPIRAM);
        if (!rec) { heap_caps_free(evt.peer_profile); continue; }
        snprintf(rec->peer_id, sizeof(rec->peer_id), "%s", peer_id);
        snprintf(rec->display_name, sizeof(rec->display_name), "%s",
                 evt.peer_profile->display_name);
        snprintf(rec->bio, sizeof(rec->bio), "%s", evt.peer_profile->bio);
        buddy_contacts_upsert(rec);
        heap_caps_free(rec);

        /* Nothing further to do with the profile: the old flow scored the two
         * people against each other from their tags/vibe/open_to and sent a
         * "Buddy Match!" notification.  Those profile fields are gone, and the
         * meeting itself is now what gets reported — the BLE chat session
         * forwards each turn and writes an encounter report of its own, which
         * says far more than a similarity score did. */
        heap_caps_free(evt.peer_profile);
        buddy_led_set(BUDDY_LED_PATTERN_OFF);
        vTaskDelay(pdMS_TO_TICKS(100));  /* brief gap */
    }
}

/* ── Init / Start ─────────────────────────────────────────────── */
esp_err_t buddy_agent_init(void)
{
    esp_err_t err = led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed (non-fatal): %s", esp_err_to_name(err));
    }
    buddy_led_set(BUDDY_LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t buddy_agent_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        buddy_contact_task, "buddy_contact",
        MIMI_BUDDY_CONTACT_STACK, NULL, MIMI_BUDDY_CONTACT_PRIO, NULL, MIMI_BUDDY_CONTACT_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create contact task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Buddy agent started on Core 1");
    return ESP_OK;
}
