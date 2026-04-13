#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adv_buffer.h"
#include "influx_upload.h"

#define TAG "influx_upload"

#define UPLOAD_INTERVAL_MS 1000
#define MAX_BATCH_POINTS 512
#define MAX_LINE_LENGTH 128
#define HTTP_TIMEOUT_MS 10000
#define RESPONSE_BUFFER_SIZE 256

typedef struct {
    app_state_t *state;
    influx_config_t config;
    advertisement_t retry_batch[MAX_BATCH_POINTS];
    size_t retry_count;
    influx_upload_metrics_t metrics;
    bool started;
} influx_upload_state_t;

static influx_upload_state_t s_uploader;

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
                           "%s%sdb=%s&precision=millisecond&accept_partial=false",
                           config->url,
                           separator,
                           config->db);

    if ((written < 0) || ((size_t) written >= url_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

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
                               (unsigned long long) batch[i].timestamp_ms);

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
    char url[INFLUX_URL_MAX_LEN + INFLUX_DB_MAX_LEN + 96];
    char auth_header[INFLUX_TOKEN_MAX_LEN + 16];
    char response[RESPONSE_BUFFER_SIZE] = { 0 };
    char *body = NULL;
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    size_t body_len;
    int read_len;

    body_len = build_line_protocol(config, batch, count, &body);
    if ((body_len == 0) || (body == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    err = build_write_url(config, url, sizeof(url));
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    snprintf(auth_header, sizeof(auth_header), "Bearer %s", config->token);

    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
    esp_http_client_set_post_field(client, body, (int) body_len);

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *http_status_out = esp_http_client_get_status_code(client);
        if ((*http_status_out < 200) || (*http_status_out >= 300)) {
            read_len = esp_http_client_read_response(client, response, sizeof(response) - 1);
            if (read_len > 0) {
                response[read_len] = '\0';
                ESP_LOGW(TAG, "Influx write failed: status=%d body=%s", *http_status_out, response);
            }
            err = ESP_FAIL;
        }
    } else {
        *http_status_out = -1;
        ESP_LOGW(TAG, "Influx HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(body);
    return err;
}

static void uploader_task(void *arg)
{
    advertisement_t batch[MAX_BATCH_POINTS];

    (void) arg;

    while (true) {
        size_t batch_count = 0;
        int http_status = -1;
        esp_err_t err;

        if ((xEventGroupGetBits(s_uploader.state->state_event_group) & ETH_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
            continue;
        }

        if (!s_uploader.state->time_synced) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
            continue;
        }

        if (s_uploader.retry_count > 0) {
            memcpy(batch, s_uploader.retry_batch, s_uploader.retry_count * sizeof(batch[0]));
            batch_count = s_uploader.retry_count;
        } else {
            batch_count = adv_buffer_drain(batch, MAX_BATCH_POINTS);
        }

        if (batch_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
            continue;
        }

        err = send_batch(&s_uploader.config, batch, batch_count, &http_status);
        s_uploader.metrics.requests_sent++;
        s_uploader.metrics.last_http_status = http_status;

        if (err == ESP_OK) {
            s_uploader.metrics.upload_successes++;
            s_uploader.metrics.points_uploaded += batch_count;
            s_uploader.retry_count = 0;
        } else {
            s_uploader.metrics.upload_failures++;
            memcpy(s_uploader.retry_batch, batch, batch_count * sizeof(batch[0]));
            s_uploader.retry_count = batch_count;
        }

        if (batch_count < MAX_BATCH_POINTS) {
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
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

    task_created = xTaskCreate(uploader_task, "influx_upload", 8192, NULL, 4, NULL);
    if (task_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_uploader.started = true;
    return ESP_OK;
}

void influx_upload_get_metrics(influx_upload_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    *metrics = s_uploader.metrics;
}
