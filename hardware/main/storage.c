#include "nvs_flash.h"

#include "storage.h"

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
