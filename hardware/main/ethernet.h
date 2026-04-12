#pragma once

#include "esp_err.h"

#include "app_state.h"

esp_err_t ethernet_init_once(app_state_t *state);
esp_err_t ethernet_connect(app_state_t *state, uint32_t timeout_ms);
void ethernet_cleanup(app_state_t *state);
