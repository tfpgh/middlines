#pragma once

#include "esp_err.h"

esp_err_t ble_scan_start(void);
void ble_scan_ensure_active(void);
