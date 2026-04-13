#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

#include "time_sync.h"

#define TAG "time_sync"

#define NTP_RETRY_INTERVAL_MS 2000
#define MIN_VALID_EPOCH 1700000000
#define NEW_YORK_TZ "EST5EDT,M3.2.0/2,M11.1.0"

static bool s_initialized;
static bool s_started;

esp_err_t time_sync_init_once(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2,
        ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    esp_err_t err;

    if (s_initialized) {
        return ESP_OK;
    }

    setenv("TZ", NEW_YORK_TZ, 1);
    tzset();

    config.start = false;
    config.server_from_dhcp = true;
    config.renew_servers_after_new_IP = true;
    config.index_of_first_server = 1;
    config.ip_event_to_renew = IP_EVENT_ETH_GOT_IP;

    err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t time_sync_start(void)
{
    esp_err_t err;

    if (s_started) {
        return ESP_OK;
    }

    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SNTP: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    return ESP_OK;
}

bool time_sync_is_valid(void)
{
    time_t now;

    time(&now);
    return now >= MIN_VALID_EPOCH;
}

esp_err_t time_sync_wait_for_valid(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms < timeout_ms) {
        if (time_sync_is_valid()) {
            return ESP_OK;
        }

        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_RETRY_INTERVAL_MS)) == ESP_OK) {
            if (time_sync_is_valid()) {
                return ESP_OK;
            }
        }

        waited_ms += NTP_RETRY_INTERVAL_MS;
    }

    return ESP_ERR_TIMEOUT;
}

uint64_t time_sync_now_ms(void)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return ((uint64_t) now.tv_sec * 1000ULL) + ((uint64_t) now.tv_usec / 1000ULL);
}
