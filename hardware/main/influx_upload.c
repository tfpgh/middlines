#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adv_buffer.h"
#include "influx_upload.h"

#define TAG "influx_upload"

#define UPLOAD_INTERVAL_MS 1000
#define MAX_BATCH_POINTS 128
#define MAX_LINE_LENGTH 128
#define HTTP_TIMEOUT_MS 10000
#define RESPONSE_BUFFER_SIZE 256
#define UPLOADER_TASK_STACK_SIZE 8192
#define MAX_CONSECUTIVE_SENDS 16

typedef struct {
    app_state_t *state;
    influx_config_t config;
    char url[INFLUX_URL_MAX_LEN + INFLUX_DB_MAX_LEN + 96];
    char auth_header[INFLUX_TOKEN_MAX_LEN + 16];
    advertisement_t batch[MAX_BATCH_POINTS];
    advertisement_t retry_batch[MAX_BATCH_POINTS];
    size_t retry_count;
    influx_upload_metrics_t metrics;
    esp_http_client_handle_t client;
    bool started;
} influx_upload_state_t;

static influx_upload_state_t s_uploader;

static void log_heap_snapshot(const char *context)
{
    ESP_LOGI(TAG,
             "Heap %s: free=%lu largest=%lu min=%lu",
             context,
             (unsigned long) esp_get_free_heap_size(),
             (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned long) esp_get_minimum_free_heap_size());
}

static void escape_tag_value(const char *input, char *output, size_t output_size)
{
    size_t out_idx = 0;

    while ((*input != '\0') && (out_idx + 2 < output_size)) {
        if ((*input == ',') || (*input == ' ') || (*input == '=')) {
            output[out_idx++] = '\\';
        }
        output[out_idx++] = *input++;
    }

    output[out_idx] = '\0';
}

static esp_err_t build_write_url(const influx_config_t *config, char *url, size_t url_size)
{
    const char *separator = strchr(config->url, '?') ? "&" : "?";
    int written = snprintf(url,
                           url_size,
                           "%s%sdb=%s&precision=microsecond&accept_partial=false&no_sync=true",
                           config->url,
                           separator,
                           config->db);

    if ((written < 0) || ((size_t) written >= url_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void reset_http_client(void)
{
    if (s_uploader.client != NULL) {
        esp_http_client_cleanup(s_uploader.client);
        s_uploader.client = NULL;
    }
}

static esp_err_t ensure_http_client(void)
{
    esp_http_client_config_t http_config = {
        .url = s_uploader.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    if (s_uploader.client != NULL) {
        return ESP_OK;
    }

    s_uploader.client = esp_http_client_init(&http_config);
    if (s_uploader.client == NULL) {
        log_heap_snapshot("HTTP client alloc failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(s_uploader.client, "Authorization", s_uploader.auth_header);
    esp_http_client_set_header(s_uploader.client, "Content-Type", "text/plain; charset=utf-8");

    return ESP_OK;
}

static size_t build_line_protocol(const influx_config_t *config,
                                  const advertisement_t *batch,
                                  size_t count,
                                  char **body_out)
{
    char escaped_node[(INFLUX_NODE_MAX_LEN * 2) + 1];
    char *body;
    size_t offset = 0;
    size_t body_size = (count * MAX_LINE_LENGTH) + 1;

    *body_out = NULL;
    escape_tag_value(config->node, escaped_node, sizeof(escaped_node));

    body = malloc(body_size);
    if (body == NULL) {
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        int written = snprintf(body + offset,
                               body_size - offset,
                               "advertisements,node=%s mac=%llui,rssi=%lli %llu\n",
                               escaped_node,
                               (unsigned long long) batch[i].mac,
                               (long long) batch[i].rssi,
                               (unsigned long long) batch[i].timestamp_us);

        if ((written < 0) || ((size_t) written >= (body_size - offset))) {
            free(body);
            return 0;
        }

        offset += (size_t) written;
    }

    *body_out = body;
    return offset;
}

static esp_err_t send_batch(const influx_config_t *config,
                            const advertisement_t *batch,
                            size_t count,
                            int *http_status_out)
{
    char response[RESPONSE_BUFFER_SIZE] = { 0 };
    char *body = NULL;
    esp_err_t err;
    size_t body_len;
    int read_len;

    body_len = build_line_protocol(config, batch, count, &body);
    if ((body_len == 0) || (body == NULL)) {
        log_heap_snapshot("line protocol alloc failed");
        return ESP_ERR_NO_MEM;
    }

    err = ensure_http_client();
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    esp_http_client_set_post_field(s_uploader.client, body, (int) body_len);

    err = esp_http_client_perform(s_uploader.client);
    if (err == ESP_OK) {
        *http_status_out = esp_http_client_get_status_code(s_uploader.client);
        if ((*http_status_out < 200) || (*http_status_out >= 300)) {
            read_len = esp_http_client_read_response(
                s_uploader.client, response, sizeof(response) - 1);
            if (read_len > 0) {
                response[read_len] = '\0';
                ESP_LOGW(TAG, "Influx write failed: status=%d body=%s", *http_status_out, response);
            }
            err = ESP_FAIL;
            reset_http_client();
        }
    } else {
        *http_status_out = -1;
        ESP_LOGW(TAG, "Influx HTTP request failed: %s", esp_err_to_name(err));
        reset_http_client();
    }

    free(body);
    return err;
}

static void uploader_task(void *arg)
{
    uint32_t last_rotate_ms = esp_log_timestamp();

    (void) arg;

    while (true) {
        size_t batch_count = 0;
        uint32_t consecutive_sends = 0;
        int http_status = -1;
        esp_err_t err;
        uint32_t now_ms;

        if ((xEventGroupGetBits(s_uploader.state->state_event_group) & ETH_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
            continue;
        }

        if (!s_uploader.state->time_synced) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
            continue;
        }

        now_ms = esp_log_timestamp();
        if ((s_uploader.retry_count == 0) && ((now_ms - last_rotate_ms) >= UPLOAD_INTERVAL_MS)) {
            if (adv_buffer_rotate_window()) {
                last_rotate_ms = now_ms;
            }
        }

        while (consecutive_sends < MAX_CONSECUTIVE_SENDS) {
            if (s_uploader.retry_count > 0) {
                memcpy(s_uploader.batch,
                       s_uploader.retry_batch,
                       s_uploader.retry_count * sizeof(s_uploader.batch[0]));
                batch_count = s_uploader.retry_count;
            } else {
                batch_count = adv_buffer_drain(s_uploader.batch, MAX_BATCH_POINTS);
            }

            if (batch_count == 0) {
                break;
            }

            err = send_batch(&s_uploader.config, s_uploader.batch, batch_count, &http_status);
            s_uploader.metrics.requests_sent++;
            s_uploader.metrics.last_http_status = http_status;
            consecutive_sends++;

            if (consecutive_sends > s_uploader.metrics.max_consecutive_sends) {
                s_uploader.metrics.max_consecutive_sends = consecutive_sends;
            }

            if (err == ESP_OK) {
                s_uploader.metrics.upload_successes++;
                s_uploader.metrics.points_uploaded += batch_count;
                s_uploader.retry_count = 0;
            } else {
                s_uploader.metrics.upload_failures++;
                memcpy(s_uploader.retry_batch,
                       s_uploader.batch,
                       batch_count * sizeof(s_uploader.batch[0]));
                s_uploader.retry_count = batch_count;
                break;
            }

            if (batch_count == MAX_BATCH_POINTS) {
                s_uploader.metrics.immediate_reflushes++;
                continue;
            }
        }

        if (consecutive_sends == 0) {
            now_ms = esp_log_timestamp();
            if ((now_ms - last_rotate_ms) < UPLOAD_INTERVAL_MS) {
                vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS - (now_ms - last_rotate_ms)));
            } else {
                taskYIELD();
            }
        } else if (consecutive_sends >= MAX_CONSECUTIVE_SENDS) {
            taskYIELD();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t influx_upload_start(app_state_t *state, const influx_config_t *config)
{
    BaseType_t task_created;

    if (s_uploader.started) {
        return ESP_OK;
    }

    memset(&s_uploader, 0, sizeof(s_uploader));
    s_uploader.state = state;
    s_uploader.config = *config;

    if (build_write_url(&s_uploader.config, s_uploader.url, sizeof(s_uploader.url)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(s_uploader.auth_header,
             sizeof(s_uploader.auth_header),
             "Bearer %s",
             s_uploader.config.token);

    log_heap_snapshot("before uploader task create");

    task_created = xTaskCreate(uploader_task,
                               "influx_upload",
                               UPLOADER_TASK_STACK_SIZE,
                               NULL,
                               4,
                               NULL);
    if (task_created != pdPASS) {
        log_heap_snapshot("uploader task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_uploader.started = true;
    log_heap_snapshot("after uploader task create");
    return ESP_OK;
}

void influx_upload_get_metrics(influx_upload_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    *metrics = s_uploader.metrics;
}
