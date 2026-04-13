#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "influx_config.h"

#define TAG "influx_cfg"

#define INFLUX_NAMESPACE "influx"
#define INFLUX_NODE_KEY "node"
#define INFLUX_URL_KEY "url"
#define INFLUX_DB_KEY "db"
#define INFLUX_TOKEN_KEY "token"

static esp_err_t read_required_string(nvs_handle_t handle,
                                      const char *key,
                                      char *buffer,
                                      size_t buffer_size)
{
    size_t required_size = buffer_size;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Missing NVS key '%s' in namespace '%s'", key, INFLUX_NAMESPACE);
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "NVS key '%s' exceeds %u bytes", key, (unsigned int) buffer_size);
    }

    if ((err == ESP_OK) && (buffer[0] == '\0')) {
        ESP_LOGE(TAG, "NVS key '%s' must not be empty", key);
        return ESP_ERR_INVALID_STATE;
    }

    return err;
}

esp_err_t influx_config_load(influx_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    err = nvs_open(INFLUX_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to open NVS namespace '%s': %s",
                 INFLUX_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    err = read_required_string(handle, INFLUX_NODE_KEY, config->node, sizeof(config->node));
    if (err == ESP_OK) {
        err = read_required_string(handle, INFLUX_URL_KEY, config->url, sizeof(config->url));
    }
    if (err == ESP_OK) {
        err = read_required_string(handle, INFLUX_DB_KEY, config->db, sizeof(config->db));
    }
    if (err == ESP_OK) {
        err = read_required_string(handle, INFLUX_TOKEN_KEY, config->token, sizeof(config->token));
    }

    nvs_close(handle);
    return err;
}
