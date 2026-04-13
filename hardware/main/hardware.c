#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "fw_update.h"

#include <golioth/client.h>

#define TAG "hardware"

#define ETH_CONNECTED_BIT BIT0
#define GOLIOTH_CONNECTED_BIT BIT1

#define OLIMEX_ETH_PHY_POWER_GPIO 12
#define OLIMEX_ETH_PHY_ADDR 0
#define OLIMEX_ETH_PHY_RESET_GPIO -1
#define OLIMEX_ETH_MDC_GPIO 23
#define OLIMEX_ETH_MDIO_GPIO 18
#define OLIMEX_ETH_CLK_GPIO EMAC_CLK_OUT_180_GPIO

#define GOLIOTH_NVS_NAMESPACE "golioth"
#define GOLIOTH_PSK_ID_KEY "psk_id"
#define GOLIOTH_PSK_KEY "psk"
#define GOLIOTH_PSK_ID_MAX_LEN 128
#define GOLIOTH_PSK_MAX_LEN 128

#define MAIN_LOOP_INTERVAL_MS 1000
#define HEARTBEAT_INTERVAL_MS 120000
#define ETHERNET_IP_TIMEOUT_MS 30000
#define GOLIOTH_CONNECT_TIMEOUT_MS 30000
#define RETRY_BACKOFF_INITIAL_MS 5000
#define RETRY_BACKOFF_MAX_MS 60000
#define PROVISIONING_RETRY_MS 60000
#define OTA_BOOT_CONFIRM_DELAY_MS 5000

static EventGroupHandle_t s_state_event_group;
static esp_netif_t *s_eth_netif;
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_eth_handle_t s_eth_handle;
static struct golioth_client *s_golioth_client;
static bool s_platform_initialized;
static bool s_eth_handlers_registered;
static bool s_ota_started;
static bool s_ota_pending_verify;
static bool s_ota_confirmed;
static uint32_t s_boot_time_ms;

static uint32_t next_backoff_delay(uint32_t current_delay)
{
    if (current_delay == 0) {
        return RETRY_BACKOFF_INITIAL_MS;
    }

    current_delay *= 2;
    if (current_delay > RETRY_BACKOFF_MAX_MS) {
        current_delay = RETRY_BACKOFF_MAX_MS;
    }

    return current_delay;
}

static esp_err_t init_nvs(void)
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

static esp_err_t load_golioth_credentials(struct golioth_client_config *config,
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

static void power_on_ethernet_phy(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(OLIMEX_ETH_PHY_POWER_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(OLIMEX_ETH_PHY_POWER_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(OLIMEX_ETH_PHY_POWER_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void eth_event_handler(void *arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    uint8_t mac_addr[6] = { 0 };
    esp_eth_handle_t eth_handle = event_data ? *(esp_eth_handle_t *) event_data : NULL;

    (void) arg;
    (void) event_base;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        if ((eth_handle != NULL)
            && (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK)) {
            ESP_LOGI(TAG,
                     "Ethernet MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0],
                     mac_addr[1],
                     mac_addr[2],
                     mac_addr[3],
                     mac_addr[4],
                     mac_addr[5]);
        }
        ESP_LOGI(TAG, "Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        xEventGroupClearBits(s_state_event_group, ETH_CONNECTED_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet stopped");
        xEventGroupClearBits(s_state_event_group, ETH_CONNECTED_BIT);
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;

    (void) arg;
    (void) event_base;
    (void) event_id;

    ESP_LOGI(TAG, "Ethernet got IP address");
    ESP_LOGI(TAG, "ETHIP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "ETHMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "ETHGW: " IPSTR, IP2STR(&event->ip_info.gw));
    xEventGroupSetBits(s_state_event_group, ETH_CONNECTED_BIT);
}

static void on_client_event(struct golioth_client *client,
                            enum golioth_client_event event,
                            void *arg)
{
    (void) client;
    (void) arg;

    if (event == GOLIOTH_CLIENT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_state_event_group, GOLIOTH_CONNECTED_BIT);
        GLTH_LOGI(TAG, "Golioth client connected");
    } else {
        xEventGroupClearBits(s_state_event_group, GOLIOTH_CONNECTED_BIT);
        GLTH_LOGW(TAG, "Golioth client disconnected");
    }
}

static esp_err_t platform_init_once(void)
{
    esp_err_t err;

    if (s_platform_initialized) {
        return ESP_OK;
    }

    s_state_event_group = xEventGroupCreate();
    if (s_state_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    s_eth_netif = esp_netif_new(&(esp_netif_config_t) ESP_NETIF_DEFAULT_ETH());
    if (s_eth_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_eth_handlers_registered = true;
    s_platform_initialized = true;
    s_boot_time_ms = esp_log_timestamp();

    return ESP_OK;
}

static void ethernet_cleanup(void)
{
    xEventGroupClearBits(s_state_event_group, ETH_CONNECTED_BIT);

    if (s_eth_handle != NULL) {
        esp_eth_stop(s_eth_handle);
    }

    if (s_eth_glue != NULL) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    if (s_eth_handle != NULL) {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }
}

static esp_err_t ethernet_connect(uint32_t timeout_ms)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    EventBits_t bits;
    esp_err_t err;

    ethernet_cleanup();
    power_on_ethernet_phy();

    ESP_LOGI(TAG, "Starting Ethernet for Olimex ESP32-POE");

    emac_config.smi_gpio.mdc_num = OLIMEX_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = OLIMEX_ETH_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio = OLIMEX_ETH_CLK_GPIO;
    mac_config.sw_reset_timeout_ms = 1000;
    mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        return ESP_ERR_NO_MEM;
    }

    phy_config.phy_addr = OLIMEX_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = OLIMEX_ETH_PHY_RESET_GPIO;
    phy_config.reset_timeout_ms = 1000;
    phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (phy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_eth_driver_install(&(esp_eth_config_t) ETH_DEFAULT_CONFIG(mac, phy), &s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        ethernet_cleanup();
        return err;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif glue");
        ethernet_cleanup();
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach Ethernet netif: %s", esp_err_to_name(err));
        ethernet_cleanup();
        return err;
    }

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        ethernet_cleanup();
        return err;
    }

    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    bits = xEventGroupWaitBits(s_state_event_group,
                               ETH_CONNECTED_BIT,
                               pdFALSE,
                               pdTRUE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & ETH_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Timed out waiting for Ethernet IP after %lu ms", (unsigned long) timeout_ms);
        ethernet_cleanup();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void golioth_client_cleanup(void)
{
    xEventGroupClearBits(s_state_event_group, GOLIOTH_CONNECTED_BIT);

    if (s_golioth_client != NULL) {
        golioth_client_destroy(s_golioth_client);
        s_golioth_client = NULL;
    }
}

static esp_err_t golioth_connect(uint32_t timeout_ms)
{
    static char psk_id[GOLIOTH_PSK_ID_MAX_LEN];
    static char psk[GOLIOTH_PSK_MAX_LEN];
    struct golioth_client_config config = { 0 };
    EventBits_t bits;
    esp_err_t err;

    if (s_golioth_client != NULL) {
        bits = xEventGroupWaitBits(s_state_event_group,
                                   GOLIOTH_CONNECTED_BIT,
                                   pdFALSE,
                                   pdTRUE,
                                   pdMS_TO_TICKS(timeout_ms));
        if ((bits & GOLIOTH_CONNECTED_BIT) != 0) {
            return ESP_OK;
        }

        GLTH_LOGW(TAG, "Timed out waiting for Golioth reconnect, recreating client");
        golioth_client_cleanup();
    }

    err = load_golioth_credentials(&config, psk_id, sizeof(psk_id), psk, sizeof(psk));
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Golioth credentials are not provisioned in NVS. Store '%s' and '%s' in namespace '%s'.",
                 GOLIOTH_PSK_ID_KEY,
                 GOLIOTH_PSK_KEY,
                 GOLIOTH_NVS_NAMESPACE);
        return err;
    }

    s_golioth_client = golioth_client_create(&config);
    if (s_golioth_client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    golioth_client_register_event_callback(s_golioth_client, on_client_event, NULL);

    GLTH_LOGI(TAG, "Waiting for Golioth connection...");
    bits = xEventGroupWaitBits(s_state_event_group,
                               GOLIOTH_CONNECTED_BIT,
                               pdFALSE,
                               pdTRUE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & GOLIOTH_CONNECTED_BIT) == 0) {
        GLTH_LOGW(TAG, "Timed out waiting for Golioth connection after %lu ms", (unsigned long) timeout_ms);
        golioth_client_cleanup();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void detect_pending_ota_state(void)
{
    esp_ota_img_states_t ota_state;

    s_ota_pending_verify = false;
    s_ota_confirmed = false;

    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) != ESP_OK) {
        return;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_ota_pending_verify = true;
        ESP_LOGI(TAG, "Running pending-verify OTA image");
    }
}

static void confirm_ota_boot_if_healthy(uint32_t now_ms)
{
    if (!s_ota_pending_verify || s_ota_confirmed) {
        return;
    }

    if ((xEventGroupGetBits(s_state_event_group) & ETH_CONNECTED_BIT) == 0) {
        return;
    }

    if ((now_ms - s_boot_time_ms) < OTA_BOOT_CONFIRM_DELAY_MS) {
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_ota_confirmed = true;
        ESP_LOGI(TAG, "Confirmed OTA image after stable Ethernet bring-up");
    } else {
        ESP_LOGE(TAG, "Failed to confirm OTA image");
    }
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *firmware_version = app_desc->version;
    uint32_t heartbeat_counter = 0;
    uint32_t last_heartbeat_ms = 0;
    uint32_t next_eth_attempt_ms = 0;
    uint32_t next_golioth_attempt_ms = 0;
    uint32_t eth_backoff_ms = 0;
    uint32_t golioth_backoff_ms = 0;
    uint32_t provisioning_backoff_ms = 0;
    bool credentials_missing = false;
    esp_err_t err;

    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }

    err = platform_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize platform services: %s", esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(PROVISIONING_RETRY_MS));
        }
    }

    detect_pending_ota_state();

    while (true) {
        uint32_t now_ms = esp_log_timestamp();
        EventBits_t bits = xEventGroupGetBits(s_state_event_group);
        bool eth_connected = (bits & ETH_CONNECTED_BIT) != 0;
        bool golioth_connected = (bits & GOLIOTH_CONNECTED_BIT) != 0;

        confirm_ota_boot_if_healthy(now_ms);

        if (!eth_connected && (now_ms >= next_eth_attempt_ms)) {
            ESP_LOGI(TAG, "Attempting Ethernet bring-up");
            err = ethernet_connect(ETHERNET_IP_TIMEOUT_MS);
            if (err == ESP_OK) {
                eth_backoff_ms = 0;
                next_eth_attempt_ms = now_ms;
            } else {
                eth_backoff_ms = next_backoff_delay(eth_backoff_ms);
                next_eth_attempt_ms = now_ms + eth_backoff_ms;
                ESP_LOGW(TAG,
                         "Ethernet bring-up failed, retry in %lu ms",
                         (unsigned long) eth_backoff_ms);
                golioth_client_cleanup();
            }
        }

        bits = xEventGroupGetBits(s_state_event_group);
        eth_connected = (bits & ETH_CONNECTED_BIT) != 0;
        golioth_connected = (bits & GOLIOTH_CONNECTED_BIT) != 0;

        if (!eth_connected) {
            if (s_golioth_client != NULL) {
                GLTH_LOGW(TAG, "Dropping Golioth client until Ethernet recovers");
                golioth_client_cleanup();
                s_ota_started = false;
            }
        } else if (!golioth_connected && (now_ms >= next_golioth_attempt_ms)) {
            err = golioth_connect(GOLIOTH_CONNECT_TIMEOUT_MS);
            if (err == ESP_OK) {
                golioth_backoff_ms = 0;
                provisioning_backoff_ms = 0;
                credentials_missing = false;
                next_golioth_attempt_ms = now_ms;

                if (!s_ota_started) {
                    golioth_fw_update_init(s_golioth_client, firmware_version);
                    s_ota_started = true;
                }
            } else {
                if ((err == ESP_ERR_NVS_NOT_FOUND) || (err == ESP_ERR_INVALID_STATE)) {
                    credentials_missing = true;
                    provisioning_backoff_ms = PROVISIONING_RETRY_MS;
                    next_golioth_attempt_ms = now_ms + provisioning_backoff_ms;
                    ESP_LOGW(TAG,
                             "Waiting for valid Golioth credentials, retry in %lu ms",
                             (unsigned long) provisioning_backoff_ms);
                } else {
                    golioth_backoff_ms = next_backoff_delay(golioth_backoff_ms);
                    next_golioth_attempt_ms = now_ms + golioth_backoff_ms;
                    GLTH_LOGW(TAG,
                              "Golioth connect failed, retry in %lu ms",
                              (unsigned long) golioth_backoff_ms);
                }
            }
        }

        if ((now_ms - last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS) {
            ESP_LOGI(TAG,
                     "Heartbeat #%lu, heap=%lu, eth=%s, golioth=%s, ota=%s%s",
                     (unsigned long) heartbeat_counter,
                     (unsigned long) esp_get_free_heap_size(),
                     eth_connected ? "up" : "down",
                     golioth_connected ? "connected" : "disconnected",
                     s_ota_started ? "ready" : "waiting",
                     credentials_missing ? ", unprovisioned" : "");
            heartbeat_counter++;
            last_heartbeat_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
