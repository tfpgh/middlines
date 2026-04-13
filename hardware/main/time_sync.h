#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t time_sync_init_once(void);
esp_err_t time_sync_start(void);
bool time_sync_is_valid(void);
esp_err_t time_sync_wait_for_valid(uint32_t timeout_ms);
uint64_t time_sync_now_ms(void);
