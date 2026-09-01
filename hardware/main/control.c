#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "control.h"

#define TAG "control"

#define CONTROL_MANIFEST_MAX_LEN 4096
#define CONTROL_HTTP_TIMEOUT_MS 10000
#define CONTROL_RETRY_INTERVAL_MS 60000
#define CONTROL_MIN_POLL_INTERVAL_S 30
#define CONTROL_MAX_POLL_INTERVAL_S 3600
#define CONTROL_VERSION_MAX_LEN 64
#define CONTROL_MANIFEST_URL_MAX_LEN 384
#define CONTROL_RESTART_NONCE_MAX_LEN 96
#define CONTROL_SHA256_HEX_LEN 64
#define CONTROL_OTA_BUFFER_SIZE 4096
#define CONTROL_TASK_STACK_SIZE 12288

#define CONTROL_NAMESPACE "control"
#define CONTROL_LAST_RESTART_NONCE_KEY "last_restart"

typedef struct {
    bool ota_failure_reported;
    uint32_t next_poll_ms;
    char current_version[CONTROL_VERSION_MAX_LEN];
    char manifest_url[CONTROL_MANIFEST_URL_MAX_LEN];
    char auth_header[CONTROL_TOKEN_MAX_LEN + 16];
    char last_restart_nonce[CONTROL_RESTART_NONCE_MAX_LEN];
    app_state_t *state;
} control_state_t;

typedef struct {
    uint32_t poll_interval_s;
    bool has_firmware;
    char firmware_version[CONTROL_VERSION_MAX_LEN];
    char firmware_url[CONTROL_MANIFEST_URL_MAX_LEN];
    char firmware_sha256[CONTROL_SHA256_HEX_LEN + 1];
    char restart_nonce[CONTROL_RESTART_NONCE_MAX_LEN];
} control_manifest_t;

static control_state_t s_control;
static char s_preboot_ota_attempted_version[CONTROL_VERSION_MAX_LEN];

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t) (now_ms - deadline_ms) >= 0;
}

static void clamp_poll_interval(control_manifest_t *manifest)
{
    if (manifest->poll_interval_s < CONTROL_MIN_POLL_INTERVAL_S) {
        manifest->poll_interval_s = CONTROL_MIN_POLL_INTERVAL_S;
    } else if (manifest->poll_interval_s > CONTROL_MAX_POLL_INTERVAL_S) {
        manifest->poll_interval_s = CONTROL_MAX_POLL_INTERVAL_S;
    }
}

static esp_err_t load_last_restart_nonce(void)
{
    nvs_handle_t handle;
    size_t len = sizeof(s_control.last_restart_nonce);
    esp_err_t err = nvs_open(CONTROL_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_control.last_restart_nonce[0] = '\0';
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, CONTROL_LAST_RESTART_NONCE_KEY, s_control.last_restart_nonce, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_control.last_restart_nonce[0] = '\0';
        return ESP_OK;
    }

    return err;
}

static esp_err_t save_last_restart_nonce(const char *nonce)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONTROL_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, CONTROL_LAST_RESTART_NONCE_KEY, nonce);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        snprintf(s_control.last_restart_nonce,
                 sizeof(s_control.last_restart_nonce),
                 "%s",
                 nonce);
    }

    return err;
}

static esp_err_t read_json_string(const cJSON *obj,
                                  const char *key,
                                  char *buffer,
                                  size_t buffer_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(item) || (item->valuestring == NULL) || (item->valuestring[0] == '\0')) {
        return ESP_FAIL;
    }

    if (snprintf(buffer, buffer_size, "%s", item->valuestring) >= (int) buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t parse_manifest(const char *payload, control_manifest_t *manifest)
{
    cJSON *root;
    const cJSON *firmware;
    const cJSON *poll_interval;

    memset(manifest, 0, sizeof(*manifest));
    manifest->poll_interval_s = CONTROL_MIN_POLL_INTERVAL_S;

    root = cJSON_Parse(payload);
    if (root == NULL) {
        return ESP_FAIL;
    }

    poll_interval = cJSON_GetObjectItemCaseSensitive(root, "poll_interval_s");
    if (cJSON_IsNumber(poll_interval)) {
        manifest->poll_interval_s = (uint32_t) poll_interval->valuedouble;
    }

    (void) read_json_string(root,
                            "restart_nonce",
                            manifest->restart_nonce,
                            sizeof(manifest->restart_nonce));

    firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        if ((read_json_string(firmware,
                              "version",
                              manifest->firmware_version,
                              sizeof(manifest->firmware_version))
             == ESP_OK)
            && (read_json_string(firmware,
                                 "url",
                                 manifest->firmware_url,
                                 sizeof(manifest->firmware_url))
                == ESP_OK)
            && (read_json_string(firmware,
                                 "sha256",
                                 manifest->firmware_sha256,
                                 sizeof(manifest->firmware_sha256))
                == ESP_OK)) {
            manifest->has_firmware = true;
        }
    }

    clamp_poll_interval(manifest);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_manifest(control_manifest_t *manifest)
{
    esp_http_client_config_t config = {
        .url = s_control.manifest_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONTROL_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    char response[CONTROL_MANIFEST_MAX_LEN];
    esp_http_client_handle_t client = esp_http_client_init(&config);
    int content_len;
    int read_len;
    esp_err_t err;
    bool mutex_locked = false;

    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", s_control.auth_header);
    esp_http_client_set_header(client, "X-Middlines-Version", s_control.current_version);

    if (s_control.state != NULL) {
        xSemaphoreTake(s_control.state->http_mutex, portMAX_DELAY);
        mutex_locked = true;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        goto cleanup;
    }

    content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    if (esp_http_client_get_status_code(client) != 200) {
        err = ESP_FAIL;
        goto cleanup;
    }

    if ((content_len > 0) && (content_len >= (int) sizeof(response))) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    read_len = esp_http_client_read_response(client, response, sizeof(response) - 1);
    if (read_len < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    response[read_len] = '\0';
    err = ESP_OK;

cleanup:
    if (mutex_locked) {
        xSemaphoreGive(s_control.state->http_mutex);
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }

    return parse_manifest(response, manifest);
}

static bool sha256_hex_matches(const char *expected, const uint8_t *actual)
{
    char actual_hex[CONTROL_SHA256_HEX_LEN + 1];

    for (size_t i = 0; i < 32; i++) {
        snprintf(&actual_hex[i * 2], 3, "%02x", actual[i]);
    }

    for (size_t i = 0; i < CONTROL_SHA256_HEX_LEN; i++) {
        if (tolower((unsigned char) expected[i]) != actual_hex[i]) {
            return false;
        }
    }

    return expected[CONTROL_SHA256_HEX_LEN] == '\0';
}

static esp_err_t perform_ota(const control_manifest_t *manifest)
{
    esp_http_client_config_t config = {
        .url = manifest->firmware_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONTROL_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = NULL;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t update_handle = 0;
    uint8_t hash[32];
    unsigned char buffer[CONTROL_OTA_BUFFER_SIZE];
    mbedtls_sha256_context sha_ctx;
    esp_err_t err = ESP_FAIL;
    int read_len;

    if (update_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    mbedtls_sha256_init(&sha_ctx);

    client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    if (esp_http_client_get_status_code(client) != 200) {
        err = ESP_FAIL;
        goto cleanup;
    }

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        goto cleanup;
    }

    mbedtls_sha256_starts(&sha_ctx, 0);

    while ((read_len = esp_http_client_read(client, (char *) buffer, sizeof(buffer))) > 0) {
        mbedtls_sha256_update(&sha_ctx, buffer, (size_t) read_len);

        err = esp_ota_write(update_handle, buffer, (size_t) read_len);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (read_len < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    mbedtls_sha256_finish(&sha_ctx, hash);

    if (!sha256_hex_matches(manifest->firmware_sha256, hash)) {
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        update_handle = 0;
        goto cleanup;
    }
    update_handle = 0;

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        goto cleanup;
    }

    esp_http_client_cleanup(client);
    mbedtls_sha256_free(&sha_ctx);
    esp_restart();
    return ESP_OK;

cleanup:
    if (update_handle != 0) {
        esp_ota_abort(update_handle);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    mbedtls_sha256_free(&sha_ctx);
    return err;
}

static esp_err_t poll_once(uint32_t now_ms)
{
    control_manifest_t manifest;
    esp_err_t err;

    if (!deadline_reached(now_ms, s_control.next_poll_ms)) {
        return ESP_OK;
    }

    err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        s_control.next_poll_ms = now_ms + CONTROL_RETRY_INTERVAL_MS;
        ESP_LOGW(TAG, "Manifest fetch failed: %s", esp_err_to_name(err));
        return err;
    }

    s_control.next_poll_ms = now_ms + (manifest.poll_interval_s * 1000U);
    if (manifest.has_firmware
        && (strcmp(manifest.firmware_version, s_control.current_version) != 0)) {
        if (strcmp(manifest.firmware_version, s_preboot_ota_attempted_version) == 0) {
            if (!s_control.ota_failure_reported) {
                ESP_LOGW(TAG,
                         "OTA %s already failed during this boot; continuing current firmware",
                         manifest.firmware_version);
                s_control.ota_failure_reported = true;
            }
        } else {
            ESP_LOGI(TAG,
                     "OTA update available %s -> %s, restarting to apply before BLE",
                     s_control.current_version,
                     manifest.firmware_version);
            esp_restart();
        }
    }

    if ((manifest.restart_nonce[0] != '\0')
        && (strcmp(manifest.restart_nonce, s_control.last_restart_nonce) != 0)) {
        err = save_last_restart_nonce(manifest.restart_nonce);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save restart nonce: %s", esp_err_to_name(err));
            return err;
        }

        esp_restart();
    }

    return ESP_OK;
}

static void control_task(void *arg)
{
    (void) arg;

    while (true) {
        EventBits_t bits = xEventGroupGetBits(s_control.state->state_event_group);
        bool eth_connected = (bits & ETH_CONNECTED_BIT) != 0;

        if (!eth_connected || !s_control.state->time_synced) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        (void) poll_once(esp_log_timestamp());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t configure_control(const control_config_t *control_config,
                                   const char *current_version)
{
    if ((control_config == NULL) || (current_version == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_control, 0, sizeof(s_control));
    if (snprintf(s_control.current_version,
                 sizeof(s_control.current_version),
                 "%s",
                 current_version)
        >= (int) sizeof(s_control.current_version)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (snprintf(s_control.manifest_url,
                 sizeof(s_control.manifest_url),
                 "%s/node/%s/manifest",
                 control_config->url,
                 control_config->node)
        >= (int) sizeof(s_control.manifest_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (snprintf(s_control.auth_header,
                 sizeof(s_control.auth_header),
                 "Bearer %s",
                 control_config->token)
        >= (int) sizeof(s_control.auth_header)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t control_check_ota_once(const control_config_t *control_config,
                                 const char *current_version)
{
    control_manifest_t manifest;
    esp_err_t err;

    err = configure_control(control_config, current_version);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Pre-BLE OTA check, current version: %s", current_version);
    err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Pre-BLE OTA manifest fetch failed: %s", esp_err_to_name(err));
        memset(&s_control, 0, sizeof(s_control));
        return err;
    }

    if (manifest.has_firmware
        && (strcmp(manifest.firmware_version, s_control.current_version) != 0)) {
        ESP_LOGI(TAG,
                 "Pre-BLE OTA update available %s -> %s, applying",
                 s_control.current_version,
                 manifest.firmware_version);
        snprintf(s_preboot_ota_attempted_version,
                 sizeof(s_preboot_ota_attempted_version),
                 "%s",
                 manifest.firmware_version);
        return perform_ota(&manifest);
    }

    ESP_LOGI(TAG, "Pre-BLE OTA check complete, firmware up to date");
    memset(&s_control, 0, sizeof(s_control));
    return ESP_OK;
}

esp_err_t control_init(app_state_t *state,
                       const control_config_t *control_config,
                       const char *current_version)
{
    BaseType_t task_created;
    esp_err_t err;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = configure_control(control_config, current_version);
    if (err != ESP_OK) {
        return err;
    }

    if (load_last_restart_nonce() != ESP_OK) {
        s_control.last_restart_nonce[0] = '\0';
    }

    s_control.state = state;
    s_control.next_poll_ms = 0;
    task_created = xTaskCreate(control_task,
                               "control",
                               CONTROL_TASK_STACK_SIZE,
                               NULL,
                               4,
                               NULL);
    if (task_created != pdPASS) {
        memset(&s_control, 0, sizeof(s_control));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
