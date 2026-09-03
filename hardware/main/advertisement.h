#pragma once

#include <stdint.h>

typedef struct {
    uint64_t timestamp_us;
    uint8_t mac[6];
    int8_t rssi;
} advertisement_t;

_Static_assert(sizeof(advertisement_t) == 16, "advertisement_t must remain compact");
