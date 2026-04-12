#include "esp_log.h"
#include "esp_ota_ops.h"

#include "ota_boot.h"

#define TAG "ota_boot"

void detect_pending_ota_state(app_state_t *state)
{
    esp_ota_img_states_t ota_state;

    state->ota_pending_verify = false;
    state->ota_confirmed = false;

    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) != ESP_OK) {
        return;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        state->ota_pending_verify = true;
        ESP_LOGI(TAG, "Running pending-verify OTA image");
    }
}

void confirm_ota_boot_if_healthy(app_state_t *state, uint32_t now_ms, uint32_t confirm_delay_ms)
{
    if (!state->ota_pending_verify || state->ota_confirmed) {
        return;
    }

    if ((xEventGroupGetBits(state->state_event_group) & ETH_CONNECTED_BIT) == 0) {
        return;
    }

    if ((now_ms - state->boot_time_ms) < confirm_delay_ms) {
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        state->ota_confirmed = true;
        ESP_LOGI(TAG, "Confirmed OTA image after stable Ethernet bring-up");
    } else {
        ESP_LOGE(TAG, "Failed to confirm OTA image");
    }
}
