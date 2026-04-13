#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "adv_buffer.h"
#include "ble_scan.h"
#include "time_sync.h"

#define TAG "ble_scan"

static bool s_started;
static bool s_scan_active;

void ble_store_config_init(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static void log_heap_snapshot(const char *context)
{
    ESP_LOGI(TAG,
             "Heap %s: free=%lu largest=%lu min=%lu",
             context,
             (unsigned long) esp_get_free_heap_size(),
             (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned long) esp_get_minimum_free_heap_size());
}

static uint64_t mac_to_u64(const uint8_t addr[6])
{
    return ((uint64_t) addr[0] << 40) | ((uint64_t) addr[1] << 32) | ((uint64_t) addr[2] << 24)
           | ((uint64_t) addr[3] << 16) | ((uint64_t) addr[4] << 8) | (uint64_t) addr[5];
}

static void start_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params = { 0 };
    int rc;

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer BLE address type: rc=%d", rc);
        return;
    }

    disc_params.passive = 1;
    disc_params.filter_duplicates = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start passive scan: rc=%d", rc);
        s_scan_active = false;
        return;
    }

    s_scan_active = true;
    ESP_LOGI(TAG, "Passive BLE scan started");
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    advertisement_t adv;

    (void) arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!time_sync_is_valid()) {
            return 0;
        }

        adv.timestamp_us = time_sync_now_us();
        adv.mac = mac_to_u64(event->disc.addr.val);
        adv.rssi = event->disc.rssi;
        adv_buffer_push(&adv);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scan_active = false;
        ESP_LOGW(TAG, "BLE scan completed; restarting");
        start_scan();
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: reason=%d", reason);
    s_scan_active = false;
}

static void on_sync(void)
{
    uint8_t addr_val[6] = { 0 };
    uint8_t addr_type;
    int rc;

    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc == 0) {
        ble_hs_id_copy_addr(addr_type, addr_val, NULL);
        ESP_LOGI(TAG,
                 "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[0],
                 addr_val[1],
                 addr_val[2],
                 addr_val[3],
                 addr_val[4],
                 addr_val[5]);
    }

    start_scan();
}

static void host_task(void *param)
{
    (void) param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_scan_start(void)
{
    esp_err_t err;

    if (s_started) {
        return ESP_OK;
    }

    log_heap_snapshot("before NimBLE init");
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE: %s", esp_err_to_name(err));
        log_heap_snapshot("after failed NimBLE init");
        return err;
    }
    log_heap_snapshot("after NimBLE init");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_store_config_init();

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    assert(ble_svc_gap_device_name_set("middlines-sniffer") == 0);
#endif

    nimble_port_freertos_init(host_task);
    s_started = true;
    log_heap_snapshot("after NimBLE host task start");
    return ESP_OK;
}
