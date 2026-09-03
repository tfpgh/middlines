#include <string.h>

#include "telemetry_protocol.h"

#define TELEMETRY_VERSION 1

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t) (value >> 8);
    output[1] = (uint8_t) value;
}

static void write_u64_be(uint8_t *output, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) {
        output[i] = (uint8_t) (value >> ((7 - i) * 8));
    }
}

size_t telemetry_encode_observations(const advertisement_t *observations,
                                     size_t count,
                                     uint8_t *payload,
                                     size_t payload_capacity)
{
    size_t payload_size;

    if ((observations == NULL) || (payload == NULL) || (count == 0)
        || (count > UINT16_MAX)) {
        return 0;
    }

    payload_size = TELEMETRY_PAYLOAD_SIZE(count);
    if (payload_capacity < payload_size) {
        return 0;
    }

    payload[0] = 'M';
    payload[1] = 'L';
    payload[2] = TELEMETRY_VERSION;
    write_u16_be(&payload[3], (uint16_t) count);

    for (size_t i = 0; i < count; i++) {
        uint8_t *record = &payload[TELEMETRY_HEADER_SIZE + (i * TELEMETRY_RECORD_SIZE)];

        write_u64_be(record, observations[i].timestamp_us / 1000ULL);
        memcpy(&record[8], observations[i].mac, sizeof(observations[i].mac));
        record[14] = (uint8_t) observations[i].rssi;
    }

    return payload_size;
}
