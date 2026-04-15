#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "control_config.h"
#include "influx_config.h"

esp_err_t control_init(const influx_config_t *influx_config,
                       const control_config_t *control_config,
                       const char *current_version);
esp_err_t control_poll(uint32_t now_ms);
