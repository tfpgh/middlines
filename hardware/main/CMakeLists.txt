set(freertos_include "${idf_path}/components/freertos/FreeRTOS-Kernel/include/freertos")

set(deps
    "golioth_sdk"
    "app_update"
    "bootloader_support"
    "nvs_flash"
    "driver"
    "esp_eth"
    "esp_netif"
    "esp_event"
    "esp_hw_support"
    "spi_flash"
)

idf_component_register(SRCS "app_main.c"
                            "cloud.c"
                            "ethernet.c"
                            "fw_update.c"
                            "ota_boot.c"
                            "provisioning.c"
                     INCLUDE_DIRS "."
                     PRIV_INCLUDE_DIRS "${freertos_include}"
                     PRIV_REQUIRES ${deps})
