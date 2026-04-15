#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
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
#include "influx_config.h"
#include "influx_upload.h"
#include "ota_boot.h"
#include "storage.h"
#include "time_sync.h"

#define TAG "app_main"

#define ADV_BUFFER_CAPACITY 1024
#define MAIN_LOOP_INTERVAL_MS 1000
#define HEARTBEAT_INTERVAL_MS 10000
#define HEARTBEAT_EVENT_INTERVAL_MS 60000
#define ETHERNET_IP_TIMEOUT_MS 30000
#define TIME_SYNC_WAIT_MS 1000
#define INFLUX_CONFIG_RETRY_MS 60000
#define CONTROL_CONFIG_RETRY_MS 60000
#define RETRY_BACKOFF_INITIAL_MS 5000
#define RETRY_BACKOFF_MAX_MS 60000
#define PROVISIONING_RETRY_MS 60000
#define OTA_BOOT_CONFIRM_DELAY_MS 5000

static app_state_t s_app_state;
static influx_config_t s_influx_config;
static control_config_t s_control_config;

static void log_heap_snapshot(const char *context)
{
    ESP_LOGW(TAG,
             "Heap %s: free=%lu largest=%lu min=%lu",
             context,
             (unsigned long) esp_get_free_heap_size(),
             (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned long) esp_get_minimum_free_heap_size());
}

static esp_err_t start_influx_pipeline(app_state_t *state, const influx_config_t *config)
{
    esp_err_t err;

    log_heap_snapshot("before influx pipeline");

    if (!adv_buffer_is_initialized() && !adv_buffer_init(ADV_BUFFER_CAPACITY)) {
        return ESP_ERR_NO_MEM;
    }

    err = ble_scan_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE scan: %s", esp_err_to_name(err));
        return err;
    }

    err = influx_upload_start(state, config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Influx uploader: %s", esp_err_to_name(err));
        return err;
    }

    state->influx_pipeline_started = true;
    log_heap_snapshot("after uploader start");
    return ESP_OK;
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

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *firmware_version = app_desc->version;
    uint32_t heartbeat_counter = 0;
    uint32_t last_heartbeat_ms = 0;
    uint32_t last_heartbeat_event_ms = 0;
    uint32_t next_eth_attempt_ms = 0;
    uint32_t next_influx_config_attempt_ms = 0;
    uint32_t next_control_config_attempt_ms = 0;
    uint32_t eth_backoff_ms = 0;
    bool influx_config_missing = false;
    bool control_config_missing = false;
    bool prev_eth_connected = false;
    bool prev_time_synced = false;
    adv_buffer_metrics_t prev_buffer_metrics = { 0 };
    influx_upload_metrics_t prev_upload_metrics = { 0 };
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

    err = time_sync_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize time sync: %s", esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }
    s_app_state.time_sync_initialized = true;
    detect_pending_ota_state(&s_app_state);

    while (true) {
        uint32_t now_ms = esp_log_timestamp();
        EventBits_t bits = xEventGroupGetBits(s_app_state.state_event_group);
        bool eth_connected = (bits & ETH_CONNECTED_BIT) != 0;

        confirm_ota_boot_if_healthy(&s_app_state, now_ms, OTA_BOOT_CONFIRM_DELAY_MS);

        if (!eth_connected && (now_ms >= next_eth_attempt_ms)) {
            ESP_LOGI(TAG, "Attempting Ethernet bring-up");
            err = ethernet_connect(&s_app_state, ETHERNET_IP_TIMEOUT_MS);
            if (err == ESP_OK) {
                eth_backoff_ms = 0;
                next_eth_attempt_ms = now_ms;
                if (!s_app_state.time_sync_started) {
                    err = time_sync_start();
                    if (err == ESP_OK) {
                        s_app_state.time_sync_started = true;
                    }
                }
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

        if (eth_connected && !s_app_state.time_synced) {
            err = time_sync_wait_for_valid(TIME_SYNC_WAIT_MS);
            if (err == ESP_OK) {
                s_app_state.time_synced = true;
                ESP_LOGI(TAG, "System time synchronized");
            }
        }

        if (s_app_state.time_synced && !s_app_state.influx_pipeline_started
            && (now_ms >= next_influx_config_attempt_ms)) {
            err = influx_config_load(&s_influx_config);
            if (err == ESP_OK) {
                influx_config_missing = false;
                err = start_influx_pipeline(&s_app_state, &s_influx_config);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG,
                             "Influx pipeline started for node '%s' and database '%s'",
                             s_influx_config.node,
                             s_influx_config.db);
                    influx_upload_enqueue_event("boot", "info", firmware_version);
                    influx_upload_enqueue_event("time_sync_ok", "info", NULL);
                    if (eth_connected) {
                        influx_upload_enqueue_event("ethernet_up", "info", NULL);
                    }
                } else {
                    next_influx_config_attempt_ms = now_ms + INFLUX_CONFIG_RETRY_MS;
                }
            } else {
                influx_config_missing = true;
                next_influx_config_attempt_ms = now_ms + INFLUX_CONFIG_RETRY_MS;
                ESP_LOGW(TAG,
                         "Waiting for valid Influx config, retry in %lu ms",
                         (unsigned long) INFLUX_CONFIG_RETRY_MS);
            }
        }

        if (s_app_state.influx_pipeline_started && !s_app_state.control_started
            && (now_ms >= next_control_config_attempt_ms)) {
            err = control_config_load(&s_control_config);
            if (err == ESP_OK) {
                control_config_missing = false;
                err = control_init(&s_app_state, &s_influx_config, &s_control_config, firmware_version);
                if (err == ESP_OK) {
                    s_app_state.control_started = true;
                    ESP_LOGI(TAG, "Control plane started for node '%s'", s_influx_config.node);
                } else {
                    next_control_config_attempt_ms = now_ms + CONTROL_CONFIG_RETRY_MS;
                }
            } else {
                control_config_missing = true;
                next_control_config_attempt_ms = now_ms + CONTROL_CONFIG_RETRY_MS;
                ESP_LOGW(TAG,
                         "Waiting for valid control config, retry in %lu ms",
                         (unsigned long) CONTROL_CONFIG_RETRY_MS);
            }
        }

        if (s_app_state.influx_pipeline_started && (eth_connected != prev_eth_connected)) {
            influx_upload_enqueue_event(eth_connected ? "ethernet_up" : "ethernet_down",
                                        eth_connected ? "info" : "warn",
                                        NULL);
        }
        if (s_app_state.influx_pipeline_started && (s_app_state.time_synced != prev_time_synced)) {
            influx_upload_enqueue_event(s_app_state.time_synced ? "time_sync_ok" : "time_sync_lost",
                                        s_app_state.time_synced ? "info" : "warn",
                                        NULL);
        }
        prev_eth_connected = eth_connected;
        prev_time_synced = s_app_state.time_synced;

        if ((now_ms - last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS) {
            adv_buffer_metrics_t buffer_metrics = { 0 };
            influx_upload_metrics_t upload_metrics = { 0 };
            uint32_t elapsed_ms = (last_heartbeat_ms == 0) ? HEARTBEAT_INTERVAL_MS : (now_ms - last_heartbeat_ms);
            uint64_t seen_delta;
            uint64_t uploaded_delta;
            uint64_t dropped_delta;
            uint64_t request_delta;
            uint64_t avg_points_per_request = 0;

            adv_buffer_get_metrics(&buffer_metrics);
            influx_upload_get_metrics(&upload_metrics);
            seen_delta = buffer_metrics.packets_seen - prev_buffer_metrics.packets_seen;
            uploaded_delta = upload_metrics.points_uploaded - prev_upload_metrics.points_uploaded;
            dropped_delta = buffer_metrics.packets_dropped - prev_buffer_metrics.packets_dropped;
            request_delta = upload_metrics.requests_sent - prev_upload_metrics.requests_sent;
            if (request_delta > 0) {
                avg_points_per_request = uploaded_delta / request_delta;
            }

            if ((now_ms - last_heartbeat_event_ms) >= HEARTBEAT_EVENT_INTERVAL_MS) {
                char heartbeat_message[128];
                int written = snprintf(heartbeat_message,
                                       sizeof(heartbeat_message),
                                       "heap=%lu adv=%lu/%lu up=%llu fail=%llu http=%d",
                                       (unsigned long) esp_get_free_heap_size(),
                                       (unsigned long) buffer_metrics.count,
                                       (unsigned long) buffer_metrics.capacity,
                                       (unsigned long long) upload_metrics.points_uploaded,
                                       (unsigned long long) upload_metrics.upload_failures,
                                       upload_metrics.last_http_status);
                if ((written > 0) && (written < (int) sizeof(heartbeat_message))) {
                    (void) influx_upload_enqueue_event("heartbeat", "info", heartbeat_message);
                    last_heartbeat_event_ms = now_ms;
                }
            }

            ESP_LOGW(TAG,
                     "Heartbeat #%lu, heap=%lu, eth=%s, control=%s%s%s, time=%s, adv=%llu/%lu drop=%llu, up=%llu fail=%llu, rate seen=%llus up=%llus drop=%llus req=%llus avg=%llu, flush=%llu maxburst=%lu http=%d",
                     (unsigned long) heartbeat_counter,
                     (unsigned long) esp_get_free_heap_size(),
                     eth_connected ? "up" : "down",
                     s_app_state.control_started ? "ready" : "waiting",
                     influx_config_missing ? ", influx-unprovisioned" : "",
                     control_config_missing ? ", control-unprovisioned" : "",
                     s_app_state.time_synced ? "synced" : "waiting",
                     (unsigned long long) buffer_metrics.packets_seen,
                     (unsigned long) buffer_metrics.count,
                     (unsigned long long) buffer_metrics.packets_dropped,
                     (unsigned long long) upload_metrics.points_uploaded,
                     (unsigned long long) upload_metrics.upload_failures,
                     (unsigned long long) ((seen_delta * 1000ULL) / elapsed_ms),
                     (unsigned long long) ((uploaded_delta * 1000ULL) / elapsed_ms),
                     (unsigned long long) ((dropped_delta * 1000ULL) / elapsed_ms),
                     (unsigned long long) ((request_delta * 1000ULL) / elapsed_ms),
                     (unsigned long long) avg_points_per_request,
                     (unsigned long long) (upload_metrics.immediate_reflushes - prev_upload_metrics.immediate_reflushes),
                     (unsigned long) upload_metrics.max_consecutive_sends,
                     upload_metrics.last_http_status);
            prev_buffer_metrics = buffer_metrics;
            prev_upload_metrics = upload_metrics;
            heartbeat_counter++;
            last_heartbeat_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
