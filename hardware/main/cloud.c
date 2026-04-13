#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "cloud.h"
#include "provisioning.h"

#define TAG "cloud"

#define GOLIOTH_PSK_ID_MAX_LEN 128
#define GOLIOTH_PSK_MAX_LEN 128

static void log_heap_snapshot(const char *context)
{
    ESP_LOGI(TAG,
             "Heap %s: free=%lu largest=%lu min=%lu",
             context,
             (unsigned long) esp_get_free_heap_size(),
             (unsigned long) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned long) esp_get_minimum_free_heap_size());
}

static void on_client_event(struct golioth_client *client,
                            enum golioth_client_event event,
                            void *arg)
{
    app_state_t *state = arg;

    (void) client;

    if (event == GOLIOTH_CLIENT_EVENT_CONNECTED) {
        xEventGroupSetBits(state->state_event_group, GOLIOTH_CONNECTED_BIT);
        GLTH_LOGI(TAG, "Golioth client connected");
        log_heap_snapshot("on Golioth connected");
    } else {
        xEventGroupClearBits(state->state_event_group, GOLIOTH_CONNECTED_BIT);
        GLTH_LOGW(TAG, "Golioth client disconnected");
        log_heap_snapshot("on Golioth disconnected");
    }
}

void golioth_client_cleanup(app_state_t *state)
{
    xEventGroupClearBits(state->state_event_group, GOLIOTH_CONNECTED_BIT);

    if (state->golioth_client != NULL) {
        log_heap_snapshot("before Golioth destroy");
        golioth_client_destroy(state->golioth_client);
        state->golioth_client = NULL;
        log_heap_snapshot("after Golioth destroy");
    }
}

esp_err_t golioth_connect(app_state_t *state, uint32_t timeout_ms)
{
    static char psk_id[GOLIOTH_PSK_ID_MAX_LEN];
    static char psk[GOLIOTH_PSK_MAX_LEN];
    struct golioth_client_config config = { 0 };
    EventBits_t bits;
    esp_err_t err;

    if (state->golioth_client != NULL) {
        bits = xEventGroupWaitBits(state->state_event_group,
                                   GOLIOTH_CONNECTED_BIT,
                                   pdFALSE,
                                   pdTRUE,
                                   pdMS_TO_TICKS(timeout_ms));
        if ((bits & GOLIOTH_CONNECTED_BIT) != 0) {
            return ESP_OK;
        }

        GLTH_LOGW(TAG, "Timed out waiting for Golioth reconnect, recreating client");
        golioth_client_cleanup(state);
    }

    err = load_golioth_credentials(&config, psk_id, sizeof(psk_id), psk, sizeof(psk));
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Golioth credentials are not provisioned in NVS. Store 'psk_id' and 'psk' in namespace 'golioth'.");
        return err;
    }

    log_heap_snapshot("before Golioth create");
    state->golioth_client = golioth_client_create(&config);
    if (state->golioth_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    log_heap_snapshot("after Golioth create");

    golioth_client_register_event_callback(state->golioth_client, on_client_event, state);

    GLTH_LOGI(TAG, "Waiting for Golioth connection...");
    bits = xEventGroupWaitBits(state->state_event_group,
                               GOLIOTH_CONNECTED_BIT,
                               pdFALSE,
                               pdTRUE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & GOLIOTH_CONNECTED_BIT) == 0) {
        GLTH_LOGW(TAG, "Timed out waiting for Golioth connection after %lu ms", (unsigned long) timeout_ms);
        golioth_client_cleanup(state);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
