set(freertos_include "${idf_path}/components/freertos/FreeRTOS-Kernel/include/freertos")

set(deps
    "app_update"
    "bootloader_support"
    "bt"
    "esp_http_client"
    "json"
    "mbedtls"
    "nvs_flash"
    "driver"
    "esp_eth"
    "esp_netif"
    "esp_event"
    "esp_hw_support"
    "lwip"
    "spi_flash"
)

idf_component_register(SRCS "app_main.c"
                             "adv_buffer.c"
                             "ble_scan.c"
                             "control.c"
                             "control_config.c"
                             "ethernet.c"
                             "influx_config.c"
                             "influx_upload.c"
                             "ota_boot.c"
                             "storage.c"
                             "time_sync.c"
                      INCLUDE_DIRS "."
                      PRIV_INCLUDE_DIRS "${freertos_include}"
                     PRIV_REQUIRES ${deps})
