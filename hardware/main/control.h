#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "app_state.h"
#include "control_config.h"

esp_err_t control_check_ota_once(const control_config_t *control_config,
                                 const char *current_version);

esp_err_t control_init(app_state_t *state,
                       const control_config_t *control_config,
                       const char *current_version);
