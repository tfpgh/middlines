#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "app_state.h"
#include "control_config.h"
#include "influx_config.h"

esp_err_t control_init(app_state_t *state,
                       const influx_config_t *influx_config,
                       const control_config_t *control_config,
                       const char *current_version);
