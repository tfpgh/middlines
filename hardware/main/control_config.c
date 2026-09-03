#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "control_config.h"

#define TAG "control_cfg"

#define CONTROL_NAMESPACE "control"
#define LEGACY_NODE_NAMESPACE "influx"
#define CONTROL_NODE_KEY "node"
#define CONTROL_URL_KEY "url"
#define CONTROL_TOKEN_KEY "token"

static esp_err_t validate_required_string(esp_err_t err,
                                          const char *namespace,
                                          const char *key,
                                          const char *buffer,
                                          size_t buffer_size)
{
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Missing NVS key '%s' in namespace '%s'", key, namespace);
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG,
                 "NVS key '%s' in namespace '%s' exceeds %u bytes",
                 key,
                 namespace,
                 (unsigned int) buffer_size);
    }

    if ((err == ESP_OK) && (buffer[0] == '\0')) {
        ESP_LOGE(TAG, "NVS key '%s' in namespace '%s' must not be empty", key, namespace);
        return ESP_ERR_INVALID_STATE;
    }

    return err;
}

static esp_err_t read_required_string(nvs_handle_t handle,
                                      const char *namespace,
                                      const char *key,
                                      char *buffer,
                                      size_t buffer_size)
{
    size_t required_size = buffer_size;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required_size);

    return validate_required_string(err, namespace, key, buffer, buffer_size);
}

static esp_err_t read_node(nvs_handle_t control_handle, char *buffer, size_t buffer_size)
{
    nvs_handle_t legacy_handle;
    size_t required_size = buffer_size;
    esp_err_t err = nvs_get_str(control_handle, CONTROL_NODE_KEY, buffer, &required_size);

    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return validate_required_string(err,
                                        CONTROL_NAMESPACE,
                                        CONTROL_NODE_KEY,
                                        buffer,
                                        buffer_size);
    }

    /* Firmware through 146dc96 stored the node name under influx/node. */
    err = nvs_open(LEGACY_NODE_NAMESPACE, NVS_READONLY, &legacy_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Node is absent from namespace '%s' and legacy namespace '%s' is unavailable",
                 CONTROL_NAMESPACE,
                 LEGACY_NODE_NAMESPACE);
        return err;
    }

    err = read_required_string(legacy_handle,
                               LEGACY_NODE_NAMESPACE,
                               CONTROL_NODE_KEY,
                               buffer,
                               buffer_size);
    nvs_close(legacy_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Using node name from legacy namespace '%s'", LEGACY_NODE_NAMESPACE);
    }
    return err;
}

esp_err_t control_config_load(control_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    err = nvs_open(CONTROL_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to open NVS namespace '%s': %s",
                 CONTROL_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    err = read_node(handle, config->node, sizeof(config->node));
    if (err == ESP_OK) {
        err = read_required_string(handle,
                                   CONTROL_NAMESPACE,
                                   CONTROL_URL_KEY,
                                   config->url,
                                   sizeof(config->url));
    }
    if (err == ESP_OK) {
        err = read_required_string(handle,
                                   CONTROL_NAMESPACE,
                                   CONTROL_TOKEN_KEY,
                                   config->token,
                                   sizeof(config->token));
    }
    nvs_close(handle);
    return err;
}
