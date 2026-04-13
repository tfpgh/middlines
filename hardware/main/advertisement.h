#pragma once

#include <stdint.h>

typedef struct {
    uint64_t timestamp_us;
    uint64_t mac;
    int32_t rssi;
} advertisement_t;
