#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_state.h"
#include "cloud.h"
#include "ethernet.h"
#include "fw_update.h"
#include "ota_boot.h"
#include "provisioning.h"

#define TAG "app_main"

#define MAIN_LOOP_INTERVAL_MS 1000
#define HEARTBEAT_INTERVAL_MS 120000
#define ETHERNET_IP_TIMEOUT_MS 30000
#define GOLIOTH_CONNECT_TIMEOUT_MS 30000
#define RETRY_BACKOFF_INITIAL_MS 5000
#define RETRY_BACKOFF_MAX_MS 60000
#define PROVISIONING_RETRY_MS 60000
#define OTA_BOOT_CONFIRM_DELAY_MS 5000

static app_state_t s_app_state;

static uint32_t next_backoff_delay(uint32_t current_delay)
{
    if (current_delay == 0) {
        return RETRY_BACKOFF_INITIAL_MS;
    }

    current_delay *= 2;
    if (current_delay > RETRY_BACKOFF_MAX_MS) {
        current_delay = RETRY_BACKOFF_MAX_MS;
    }

    return current_delay;
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *firmware_version = app_desc->version;
    uint32_t heartbeat_counter = 0;
    uint32_t last_heartbeat_ms = 0;
    uint32_t next_eth_attempt_ms = 0;
    uint32_t next_golioth_attempt_ms = 0;
    uint32_t eth_backoff_ms = 0;
    uint32_t golioth_backoff_ms = 0;
    uint32_t provisioning_backoff_ms = 0;
    bool credentials_missing = false;
    esp_err_t err;

    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }

    err = ethernet_init_once(&s_app_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize platform services: %s", esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }

    detect_pending_ota_state(&s_app_state);

    while (true) {
        uint32_t now_ms = esp_log_timestamp();
        EventBits_t bits = xEventGroupGetBits(s_app_state.state_event_group);
        bool eth_connected = (bits & ETH_CONNECTED_BIT) != 0;
        bool golioth_connected = (bits & GOLIOTH_CONNECTED_BIT) != 0;

        confirm_ota_boot_if_healthy(&s_app_state, now_ms, OTA_BOOT_CONFIRM_DELAY_MS);

        if (!eth_connected && (now_ms >= next_eth_attempt_ms)) {
            ESP_LOGI(TAG, "Attempting Ethernet bring-up");
            err = ethernet_connect(&s_app_state, ETHERNET_IP_TIMEOUT_MS);
            if (err == ESP_OK) {
                eth_backoff_ms = 0;
                next_eth_attempt_ms = now_ms;
            } else {
                eth_backoff_ms = next_backoff_delay(eth_backoff_ms);
                next_eth_attempt_ms = now_ms + eth_backoff_ms;
                ESP_LOGW(TAG,
                         "Ethernet bring-up failed, retry in %lu ms",
                         (unsigned long) eth_backoff_ms);
                golioth_client_cleanup(&s_app_state);
            }
        }

        bits = xEventGroupGetBits(s_app_state.state_event_group);
        eth_connected = (bits & ETH_CONNECTED_BIT) != 0;
        golioth_connected = (bits & GOLIOTH_CONNECTED_BIT) != 0;

        if (!eth_connected) {
            if (s_app_state.golioth_client != NULL) {
                GLTH_LOGW(TAG, "Dropping Golioth client until Ethernet recovers");
                golioth_client_cleanup(&s_app_state);
                s_app_state.ota_started = false;
            }
        } else if (!golioth_connected && (now_ms >= next_golioth_attempt_ms)) {
            err = golioth_connect(&s_app_state, GOLIOTH_CONNECT_TIMEOUT_MS);
            if (err == ESP_OK) {
                golioth_backoff_ms = 0;
                provisioning_backoff_ms = 0;
                credentials_missing = false;
                next_golioth_attempt_ms = now_ms;

                if (!s_app_state.ota_started) {
                    golioth_fw_update_init(s_app_state.golioth_client, firmware_version);
                    s_app_state.ota_started = true;
                }
            } else {
                if ((err == ESP_ERR_NVS_NOT_FOUND) || (err == ESP_ERR_INVALID_STATE)) {
                    credentials_missing = true;
                    provisioning_backoff_ms = PROVISIONING_RETRY_MS;
                    next_golioth_attempt_ms = now_ms + provisioning_backoff_ms;
                    ESP_LOGW(TAG,
                             "Waiting for valid Golioth credentials, retry in %lu ms",
                             (unsigned long) provisioning_backoff_ms);
                } else {
                    golioth_backoff_ms = next_backoff_delay(golioth_backoff_ms);
                    next_golioth_attempt_ms = now_ms + golioth_backoff_ms;
                    GLTH_LOGW(TAG,
                              "Golioth connect failed, retry in %lu ms",
                              (unsigned long) golioth_backoff_ms);
                }
            }
        }

        if ((now_ms - last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS) {
            ESP_LOGI(TAG,
                     "Heartbeat #%lu, heap=%lu, eth=%s, golioth=%s, ota=%s%s",
                     (unsigned long) heartbeat_counter,
                     (unsigned long) esp_get_free_heap_size(),
                     eth_connected ? "up" : "down",
                     golioth_connected ? "connected" : "disconnected",
                     s_app_state.ota_started ? "ready" : "waiting",
                     credentials_missing ? ", unprovisioned" : "");
            heartbeat_counter++;
            last_heartbeat_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
