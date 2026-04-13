#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"

#include <golioth/client.h>

#define ETH_CONNECTED_BIT BIT0
#define GOLIOTH_CONNECTED_BIT BIT1

typedef struct {
    EventGroupHandle_t state_event_group;
    esp_netif_t *eth_netif;
    esp_eth_netif_glue_handle_t eth_glue;
    esp_eth_handle_t eth_handle;
    struct golioth_client *golioth_client;
    bool platform_initialized;
    bool time_sync_initialized;
    bool time_sync_started;
    bool time_synced;
    bool ota_started;
    bool ota_pending_verify;
    bool ota_confirmed;
    bool influx_pipeline_started;
    uint32_t boot_time_ms;
} app_state_t;
