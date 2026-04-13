#pragma once

#include <stdint.h>

typedef struct {
    uint64_t timestamp_ms;
    uint64_t mac;
    int32_t rssi;
} advertisement_t;
