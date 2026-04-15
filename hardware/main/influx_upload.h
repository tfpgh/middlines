#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_state.h"
#include "influx_config.h"

typedef struct {
    uint64_t upload_successes;
    uint64_t upload_failures;
    uint64_t points_uploaded;
    uint64_t requests_sent;
    uint64_t immediate_reflushes;
    uint32_t max_consecutive_sends;
    int last_http_status;
} influx_upload_metrics_t;

esp_err_t influx_upload_start(app_state_t *state, const influx_config_t *config);
esp_err_t influx_upload_log_event(const char *event, const char *status, const char *message);
void influx_upload_get_metrics(influx_upload_metrics_t *metrics);
