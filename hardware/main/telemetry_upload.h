#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "app_state.h"
#include "control_config.h"

typedef struct {
    uint64_t upload_failures;
    uint64_t observations_uploaded;
} telemetry_upload_metrics_t;

esp_err_t telemetry_upload_start(app_state_t *state, const control_config_t *config);
void telemetry_upload_get_metrics(telemetry_upload_metrics_t *metrics);
