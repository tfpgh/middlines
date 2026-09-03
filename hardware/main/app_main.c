#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adv_buffer.h"
#include "app_state.h"
#include "ble_scan.h"
#include "control.h"
#include "control_config.h"
#include "ethernet.h"
#include "ota_boot.h"
#include "storage.h"
#include "telemetry_upload.h"
#include "time_sync.h"

#define TAG "app_main"

#define MAIN_LOOP_INTERVAL_MS 1000
#define HEARTBEAT_INTERVAL_MS 10000
#define ETHERNET_IP_TIMEOUT_MS 30000
#define ETHERNET_NO_IP_RESTART_MS (15U * 60U * 1000U)
#define TIME_SYNC_WAIT_MS 1000
#define SERVICE_START_RETRY_MS 60000
#define RETRY_BACKOFF_INITIAL_MS 5000
#define RETRY_BACKOFF_MAX_MS 60000
#define PROVISIONING_RETRY_MS 60000
#define OTA_BOOT_CONFIRM_DELAY_MS 5000
#define OTA_CHECK_TASK_STACK_SIZE 20480

static app_state_t s_app_state;

typedef struct {
    app_state_t *state;
    const char *firmware_version;
} ota_check_task_arg_t;

static void ota_check_task(void *arg)
{
    ota_check_task_arg_t *ctx = (ota_check_task_arg_t *) arg;
    control_config_t control_config;
    esp_err_t err;

    err = control_config_load(&control_config);
    if (err == ESP_OK) {
        err = control_check_ota_once(&control_config, ctx->firmware_version);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Initial OTA check failed (%s), continuing startup",
                     esp_err_to_name(err));
        }
    }

    xEventGroupSetBits(ctx->state->state_event_group, OTA_CHECK_DONE_BIT);
    vTaskDelete(NULL);
}

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

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t) (now_ms - deadline_ms) >= 0;
}

static esp_err_t start_telemetry(app_state_t *state, const control_config_t *config)
{
    esp_err_t err;

    err = telemetry_upload_start(state, config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start telemetry uploader: %s", esp_err_to_name(err));
        return err;
    }

    err = ble_scan_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE scan: %s", esp_err_to_name(err));
        return err;
    }

    state->telemetry_started = true;
    return ESP_OK;
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *firmware_version = app_desc->version;
    uint32_t heartbeat_counter = 0;
    uint32_t last_heartbeat_ms = 0;
    uint32_t next_eth_attempt_ms = 0;
    uint32_t next_service_start_attempt_ms = 0;
    uint32_t no_ip_since_ms = 0;
    uint32_t eth_backoff_ms = 0;
    bool no_ip_timer_active = false;
    bool ota_check_started = false;
    bool service_config_missing = false;
    ota_check_task_arg_t ota_arg;
    control_config_t control_config;
    esp_err_t err;

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

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

    s_app_state.http_mutex = xSemaphoreCreateMutex();
    if (s_app_state.http_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP mutex");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }

    err = time_sync_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize time sync: %s", esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }
    detect_pending_ota_state(&s_app_state);

    while (true) {
        uint32_t now_ms = esp_log_timestamp();
        EventBits_t bits = xEventGroupGetBits(s_app_state.state_event_group);
        bool eth_connected = (bits & ETH_CONNECTED_BIT) != 0;

        if (eth_connected) {
            no_ip_timer_active = false;
        } else if (!no_ip_timer_active) {
            no_ip_since_ms = now_ms;
            no_ip_timer_active = true;
        } else if ((now_ms - no_ip_since_ms) >= ETHERNET_NO_IP_RESTART_MS) {
            ESP_LOGE(TAG,
                     "No Ethernet IP for %lu ms; restarting",
                     (unsigned long) ETHERNET_NO_IP_RESTART_MS);
            esp_restart();
        }

        confirm_ota_boot_if_healthy(&s_app_state, now_ms, OTA_BOOT_CONFIRM_DELAY_MS);

        if (!eth_connected && deadline_reached(now_ms, next_eth_attempt_ms)) {
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
            }
        }

        bits = xEventGroupGetBits(s_app_state.state_event_group);
        eth_connected = (bits & ETH_CONNECTED_BIT) != 0;

        if (eth_connected && !s_app_state.time_sync_started) {
            err = time_sync_start();
            if (err == ESP_OK) {
                s_app_state.time_sync_started = true;
            }
        }

        if (eth_connected && !s_app_state.time_synced) {
            err = time_sync_wait_for_valid(TIME_SYNC_WAIT_MS);
            if (err == ESP_OK) {
                s_app_state.time_synced = true;
                ESP_LOGI(TAG, "System time synchronized");
            }
        }

        if (s_app_state.time_synced && !ota_check_started) {
            ota_arg.state = &s_app_state;
            ota_arg.firmware_version = firmware_version;
            if (xTaskCreate(ota_check_task,
                            "ota_check",
                            OTA_CHECK_TASK_STACK_SIZE,
                            &ota_arg,
                            4,
                            NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to create OTA check task, skipping");
                xEventGroupSetBits(s_app_state.state_event_group, OTA_CHECK_DONE_BIT);
            }
            ota_check_started = true;
        }

        bits = xEventGroupGetBits(s_app_state.state_event_group);
        if (s_app_state.time_synced && ((bits & OTA_CHECK_DONE_BIT) != 0)
            && (!s_app_state.telemetry_started || !s_app_state.control_started)
            && deadline_reached(now_ms, next_service_start_attempt_ms)) {
            err = control_config_load(&control_config);
            if (err == ESP_OK) {
                service_config_missing = false;

                if (!s_app_state.telemetry_started) {
                    err = start_telemetry(&s_app_state, &control_config);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "Telemetry started for node '%s'", control_config.node);
                    }
                }

                if (!s_app_state.control_started) {
                    err = control_init(&s_app_state, &control_config, firmware_version);
                    if (err == ESP_OK) {
                        s_app_state.control_started = true;
                        ESP_LOGI(TAG, "Control plane started for node '%s'", control_config.node);
                    } else {
                        ESP_LOGE(TAG, "Failed to start control plane: %s", esp_err_to_name(err));
                    }
                }
            } else {
                service_config_missing = true;
                ESP_LOGW(TAG,
                         "Waiting for valid node config, retry in %lu ms",
                         (unsigned long) SERVICE_START_RETRY_MS);
            }

            if (!s_app_state.telemetry_started || !s_app_state.control_started) {
                next_service_start_attempt_ms = now_ms + SERVICE_START_RETRY_MS;
            }
        }

        if (s_app_state.telemetry_started) {
            ble_scan_ensure_active();
        }

        if ((now_ms - last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS) {
            adv_buffer_metrics_t buffer_metrics = { 0 };
            telemetry_upload_metrics_t upload_metrics = { 0 };

            adv_buffer_get_metrics(&buffer_metrics);
            telemetry_upload_get_metrics(&upload_metrics);
            ESP_LOGW(TAG,
                     "Heartbeat #%lu, heap=%lu min_heap=%lu, eth=%s, time=%s, control=%s%s, telemetry=%s%s, adv=%llu/%lu drop=%llu, uploaded=%llu fail=%llu",
                     (unsigned long) heartbeat_counter,
                     (unsigned long) esp_get_free_heap_size(),
                     (unsigned long) esp_get_minimum_free_heap_size(),
                     eth_connected ? "up" : "down",
                     s_app_state.time_synced ? "synced" : "waiting",
                     s_app_state.control_started ? "ready" : "waiting",
                     service_config_missing ? ", unprovisioned" : "",
                     s_app_state.telemetry_started ? "ready" : "waiting",
                     service_config_missing ? ", unprovisioned" : "",
                     (unsigned long long) buffer_metrics.packets_seen,
                     (unsigned long) buffer_metrics.count,
                     (unsigned long long) buffer_metrics.packets_dropped,
                     (unsigned long long) upload_metrics.observations_uploaded,
                     (unsigned long long) upload_metrics.upload_failures);
            heartbeat_counter++;
            last_heartbeat_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
