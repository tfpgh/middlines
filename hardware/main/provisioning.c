#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "provisioning.h"

#define TAG "provisioning"

#define GOLIOTH_NVS_NAMESPACE "golioth"
#define GOLIOTH_PSK_ID_KEY "psk_id"
#define GOLIOTH_PSK_KEY "psk"

static esp_err_t read_nvs_string(nvs_handle_t handle,
                                 const char *key,
                                 char *buffer,
                                 size_t buffer_size)
{
    size_t required_size = buffer_size;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Missing NVS key '%s' in namespace '%s'", key, GOLIOTH_NVS_NAMESPACE);
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "NVS key '%s' exceeds %u bytes", key, (unsigned int) buffer_size);
    }

    return err;
}

esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }

        err = nvs_flash_init();
    }

    return err;
}

esp_err_t load_golioth_credentials(struct golioth_client_config *config,
                                   char *psk_id,
                                   size_t psk_id_size,
                                   char *psk,
                                   size_t psk_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GOLIOTH_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to open NVS namespace '%s': %s",
                 GOLIOTH_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    err = read_nvs_string(handle, GOLIOTH_PSK_ID_KEY, psk_id, psk_id_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = read_nvs_string(handle, GOLIOTH_PSK_KEY, psk, psk_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    if ((psk_id[0] == '\0') || (psk[0] == '\0')) {
        ESP_LOGE(TAG, "Provisioned Golioth credentials must not be empty");
        return ESP_ERR_INVALID_STATE;
    }

    config->credentials.auth_type = GOLIOTH_TLS_AUTH_TYPE_PSK;
    config->credentials.psk.psk_id = psk_id;
    config->credentials.psk.psk_id_len = strlen(psk_id);
    config->credentials.psk.psk = psk;
    config->credentials.psk.psk_len = strlen(psk);

    return ESP_OK;
}
