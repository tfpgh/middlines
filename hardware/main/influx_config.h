#pragma once

#include "esp_err.h"

#define INFLUX_NODE_MAX_LEN 64
#define INFLUX_URL_MAX_LEN 256
#define INFLUX_DB_MAX_LEN 64
#define INFLUX_TOKEN_MAX_LEN 256

typedef struct {
    char node[INFLUX_NODE_MAX_LEN];
    char url[INFLUX_URL_MAX_LEN];
    char db[INFLUX_DB_MAX_LEN];
    char token[INFLUX_TOKEN_MAX_LEN];
} influx_config_t;

esp_err_t influx_config_load(influx_config_t *config);
