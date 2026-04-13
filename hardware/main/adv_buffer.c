#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "adv_buffer.h"

typedef struct {
    advertisement_t *entries;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    uint64_t packets_seen;
    uint64_t packets_enqueued;
    uint64_t packets_dropped;
    uint32_t high_watermark;
    portMUX_TYPE lock;
} adv_buffer_state_t;

static adv_buffer_state_t s_buffer = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

bool adv_buffer_is_initialized(void)
{
    return s_buffer.entries != NULL;
}

bool adv_buffer_init(size_t capacity)
{
    if (adv_buffer_is_initialized()) {
        return true;
    }

    s_buffer.entries = calloc(capacity, sizeof(advertisement_t));
    if (s_buffer.entries == NULL) {
        return false;
    }

    s_buffer.capacity = capacity;
    return true;
}

bool adv_buffer_push(const advertisement_t *adv)
{
    bool stored = true;

    if (!adv_buffer_is_initialized() || (adv == NULL)) {
        return false;
    }

    portENTER_CRITICAL(&s_buffer.lock);

    s_buffer.packets_seen++;
    if (s_buffer.count == s_buffer.capacity) {
        s_buffer.tail = (s_buffer.tail + 1) % s_buffer.capacity;
        s_buffer.count--;
        s_buffer.packets_dropped++;
        stored = false;
    }

    s_buffer.entries[s_buffer.head] = *adv;
    s_buffer.head = (s_buffer.head + 1) % s_buffer.capacity;
    s_buffer.count++;
    s_buffer.packets_enqueued++;
    if (s_buffer.count > s_buffer.high_watermark) {
        s_buffer.high_watermark = (uint32_t) s_buffer.count;
    }

    portEXIT_CRITICAL(&s_buffer.lock);
    return stored;
}

size_t adv_buffer_drain(advertisement_t *out, size_t max_items)
{
    size_t drained = 0;

    if (!adv_buffer_is_initialized() || (out == NULL) || (max_items == 0)) {
        return 0;
    }

    portENTER_CRITICAL(&s_buffer.lock);
    while ((drained < max_items) && (s_buffer.count > 0)) {
        out[drained] = s_buffer.entries[s_buffer.tail];
        s_buffer.tail = (s_buffer.tail + 1) % s_buffer.capacity;
        s_buffer.count--;
        drained++;
    }
    portEXIT_CRITICAL(&s_buffer.lock);

    return drained;
}

void adv_buffer_get_metrics(adv_buffer_metrics_t *metrics)
{
    if (!adv_buffer_is_initialized() || (metrics == NULL)) {
        return;
    }

    portENTER_CRITICAL(&s_buffer.lock);
    memset(metrics, 0, sizeof(*metrics));
    metrics->packets_seen = s_buffer.packets_seen;
    metrics->packets_enqueued = s_buffer.packets_enqueued;
    metrics->packets_dropped = s_buffer.packets_dropped;
    metrics->high_watermark = s_buffer.high_watermark;
    metrics->count = (uint32_t) s_buffer.count;
    metrics->capacity = (uint32_t) s_buffer.capacity;
    portEXIT_CRITICAL(&s_buffer.lock);
}
