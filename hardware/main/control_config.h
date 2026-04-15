#pragma once

#include "esp_err.h"

#define CONTROL_URL_MAX_LEN 256
#define CONTROL_TOKEN_MAX_LEN 256

typedef struct {
    char url[CONTROL_URL_MAX_LEN];
    char token[CONTROL_TOKEN_MAX_LEN];
} control_config_t;

esp_err_t control_config_load(control_config_t *config);
