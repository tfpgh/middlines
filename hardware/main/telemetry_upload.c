#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adv_buffer.h"
#include "telemetry_protocol.h"
#include "telemetry_upload.h"

#define TAG "telemetry_upload"

#define UPLOAD_INTERVAL_MS 1000
#define UPLOAD_RETRY_DELAY_MS 1000
#define UPLOAD_HTTP_TIMEOUT_MS 10000
#define UPLOAD_TASK_STACK_SIZE 8192
#define MAX_BATCH_OBSERVATIONS 256
#define UPLOAD_URL_MAX_LEN (CONTROL_URL_MAX_LEN + CONTROL_NODE_MAX_LEN + 32)

typedef struct {
    app_state_t *state;
    esp_http_client_handle_t client;
    char url[UPLOAD_URL_MAX_LEN];
    char auth_header[CONTROL_TOKEN_MAX_LEN + 16];
    advertisement_t batch[MAX_BATCH_OBSERVATIONS];
    uint8_t payload[TELEMETRY_PAYLOAD_SIZE(MAX_BATCH_OBSERVATIONS)];
    telemetry_upload_metrics_t metrics;
    bool started;
} telemetry_upload_state_t;

static telemetry_upload_state_t s_uploader;

static esp_err_t upload_batch(const advertisement_t *observations, size_t count)
{
    size_t payload_size = telemetry_encode_observations(observations,
                                                        count,
                                                        s_uploader.payload,
                                                        sizeof(s_uploader.payload));
    esp_err_t err;

    if (payload_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_uploader.state->http_mutex, portMAX_DELAY);
    err = esp_http_client_set_post_field(s_uploader.client,
                                         (const char *) s_uploader.payload,
                                         (int) payload_size);
    if (err == ESP_OK) {
        err = esp_http_client_perform(s_uploader.client);
    }
    if ((err == ESP_OK) && (esp_http_client_get_status_code(s_uploader.client) != 204)) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        esp_http_client_close(s_uploader.client);
    }
    xSemaphoreGive(s_uploader.state->http_mutex);

    return err;
}

static void uploader_task(void *arg)
{
    uint32_t last_rotate_ms = esp_log_timestamp();

    (void) arg;

    while (true) {
        EventBits_t bits = xEventGroupGetBits(s_uploader.state->state_event_group);
        uint32_t now_ms;
        size_t batch_count;
        esp_err_t err;

        if ((bits & ETH_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY_MS));
            continue;
        }

        now_ms = esp_log_timestamp();
        if (((now_ms - last_rotate_ms) >= UPLOAD_INTERVAL_MS)
            && adv_buffer_rotate_window()) {
            last_rotate_ms = now_ms;
        }

        batch_count = adv_buffer_drain(s_uploader.batch, MAX_BATCH_OBSERVATIONS);
        if (batch_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        err = upload_batch(s_uploader.batch, batch_count);
        if (err == ESP_OK) {
            s_uploader.metrics.observations_uploaded += batch_count;
        } else {
            s_uploader.metrics.upload_failures++;
            ESP_LOGW(TAG,
                     "Upload failed; dropped %u observations: %s",
                     (unsigned int) batch_count,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY_MS));
        }
        taskYIELD();
    }
}

esp_err_t telemetry_upload_start(app_state_t *state, const control_config_t *config)
{
    esp_http_client_config_t http_config;
    BaseType_t task_created;
    esp_err_t err;

    if ((state == NULL) || (config == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_uploader.started) {
        return ESP_OK;
    }

    memset(&s_uploader, 0, sizeof(s_uploader));
    s_uploader.state = state;
    if (snprintf(s_uploader.url,
                 sizeof(s_uploader.url),
                 "%s/node/%s/observations",
                 config->url,
                 config->node)
        >= (int) sizeof(s_uploader.url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (snprintf(s_uploader.auth_header,
                 sizeof(s_uploader.auth_header),
                 "Bearer %s",
                 config->token)
        >= (int) sizeof(s_uploader.auth_header)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&http_config, 0, sizeof(http_config));
    http_config.url = s_uploader.url;
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = UPLOAD_HTTP_TIMEOUT_MS;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;

    s_uploader.client = esp_http_client_init(&http_config);
    if (s_uploader.client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_set_header(s_uploader.client,
                                     "Authorization",
                                     s_uploader.auth_header);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(s_uploader.client,
                                         "Content-Type",
                                         "application/octet-stream");
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(s_uploader.client);
        s_uploader.client = NULL;
        return err;
    }

    task_created = xTaskCreate(uploader_task,
                               "telemetry_upload",
                               UPLOAD_TASK_STACK_SIZE,
                               NULL,
                               4,
                               NULL);
    if (task_created != pdPASS) {
        esp_http_client_cleanup(s_uploader.client);
        s_uploader.client = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_uploader.started = true;
    ESP_LOGI(TAG, "Uploading observations to %s", s_uploader.url);
    return ESP_OK;
}

void telemetry_upload_get_metrics(telemetry_upload_metrics_t *metrics)
{
    if (metrics != NULL) {
        *metrics = s_uploader.metrics;
    }
}
