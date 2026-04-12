#pragma once

#include "app_state.h"

void detect_pending_ota_state(app_state_t *state);
void confirm_ota_boot_if_healthy(app_state_t *state, uint32_t now_ms, uint32_t confirm_delay_ms);
