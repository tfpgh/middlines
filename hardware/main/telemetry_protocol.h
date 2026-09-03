#pragma once

#include <stddef.h>
#include <stdint.h>

#include "advertisement.h"

#define TELEMETRY_HEADER_SIZE 5
#define TELEMETRY_RECORD_SIZE 15
#define TELEMETRY_PAYLOAD_SIZE(record_count) \
    (TELEMETRY_HEADER_SIZE + ((record_count) * TELEMETRY_RECORD_SIZE))

size_t telemetry_encode_observations(const advertisement_t *observations,
                                     size_t count,
                                     uint8_t *payload,
                                     size_t payload_capacity);
