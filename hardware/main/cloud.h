#pragma once

#include "esp_err.h"

#include "app_state.h"

esp_err_t golioth_connect(app_state_t *state, uint32_t timeout_ms);
void golioth_client_cleanup(app_state_t *state);
