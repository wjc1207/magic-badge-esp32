#include "buddy.h"
#include "buddy_profile.h"
#include "buddy_contacts.h"
#include "buddy_ble.h"
#include "buddy_chat.h"
#include "buddy_agent.h"

#include "esp_log.h"

static const char *TAG = "buddy";

esp_err_t buddy_init(void)
{
    esp_err_t err;

    /* 1. Load/generate device identity + user profile */
    err = buddy_profile_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Profile init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Mount contact store */
    err = buddy_contacts_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Contacts init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Initialize BLE transport */
    err = buddy_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Initialize agent (LED, etc.) */
    err = buddy_agent_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Agent init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 5. Initialize the chat session module */
    err = buddy_chat_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Chat init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Buddy subsystem initialized");
    return ESP_OK;
}

esp_err_t buddy_start(void)
{
    esp_err_t err;

    /* Start BLE advertising + scanning */
    err = buddy_ble_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start contact processing task */
    err = buddy_agent_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Agent start failed: %s", esp_err_to_name(err));
        buddy_ble_stop();
        return err;
    }

    /* Start conversation sessions */
    err = buddy_chat_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Chat start failed: %s", esp_err_to_name(err));
        buddy_ble_stop();
        return err;
    }

    ESP_LOGI(TAG, "Buddy subsystem started — discovering peers");
    return ESP_OK;
}

esp_err_t buddy_stop(void)
{
    buddy_chat_stop();
    buddy_ble_stop();
    buddy_led_set(BUDDY_LED_PATTERN_OFF);
    ESP_LOGI(TAG, "Buddy subsystem stopped");
    return ESP_OK;
}
