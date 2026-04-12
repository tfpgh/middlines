#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ethernet.h"

#define TAG "ethernet"

#define OLIMEX_ETH_PHY_POWER_GPIO 12
#define OLIMEX_ETH_PHY_ADDR 0
#define OLIMEX_ETH_PHY_RESET_GPIO -1
#define OLIMEX_ETH_MDC_GPIO 23
#define OLIMEX_ETH_MDIO_GPIO 18
#define OLIMEX_ETH_CLK_GPIO EMAC_CLK_OUT_180_GPIO

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
    app_state_t *state = arg;
    esp_eth_handle_t eth_handle = event_data ? *(esp_eth_handle_t *) event_data : NULL;

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
        xEventGroupClearBits(state->state_event_group, ETH_CONNECTED_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet stopped");
        xEventGroupClearBits(state->state_event_group, ETH_CONNECTED_BIT);
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
    app_state_t *state = arg;

    (void) event_base;
    (void) event_id;

    ESP_LOGI(TAG, "Ethernet got IP address");
    ESP_LOGI(TAG, "ETHIP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "ETHMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "ETHGW: " IPSTR, IP2STR(&event->ip_info.gw));
    xEventGroupSetBits(state->state_event_group, ETH_CONNECTED_BIT);
}

esp_err_t ethernet_init_once(app_state_t *state)
{
    esp_err_t err;

    if (state->platform_initialized) {
        return ESP_OK;
    }

    state->state_event_group = xEventGroupCreate();
    if (state->state_event_group == NULL) {
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

    state->eth_netif = esp_netif_new(&(esp_netif_config_t) ESP_NETIF_DEFAULT_ETH());
    if (state->eth_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, state);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, state);
    if (err != ESP_OK) {
        return err;
    }

    state->platform_initialized = true;
    state->boot_time_ms = esp_log_timestamp();

    return ESP_OK;
}

void ethernet_cleanup(app_state_t *state)
{
    xEventGroupClearBits(state->state_event_group, ETH_CONNECTED_BIT);

    if (state->eth_handle != NULL) {
        esp_eth_stop(state->eth_handle);
    }

    if (state->eth_glue != NULL) {
        esp_eth_del_netif_glue(state->eth_glue);
        state->eth_glue = NULL;
    }

    if (state->eth_handle != NULL) {
        esp_eth_driver_uninstall(state->eth_handle);
        state->eth_handle = NULL;
    }
}

esp_err_t ethernet_connect(app_state_t *state, uint32_t timeout_ms)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    EventBits_t bits;
    esp_err_t err;

    ethernet_cleanup(state);
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

    err = esp_eth_driver_install(&(esp_eth_config_t) ETH_DEFAULT_CONFIG(mac, phy), &state->eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        ethernet_cleanup(state);
        return err;
    }

    state->eth_glue = esp_eth_new_netif_glue(state->eth_handle);
    if (state->eth_glue == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif glue");
        ethernet_cleanup(state);
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(state->eth_netif, state->eth_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach Ethernet netif: %s", esp_err_to_name(err));
        ethernet_cleanup(state);
        return err;
    }

    err = esp_eth_start(state->eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        ethernet_cleanup(state);
        return err;
    }

    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    bits = xEventGroupWaitBits(state->state_event_group,
                               ETH_CONNECTED_BIT,
                               pdFALSE,
                               pdTRUE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & ETH_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Timed out waiting for Ethernet IP after %lu ms", (unsigned long) timeout_ms);
        ethernet_cleanup(state);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
