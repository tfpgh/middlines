#include <string.h>

#include "freertos/FreeRTOS.h"

#include "adv_buffer.h"

#define ADV_BUFFER_CAPACITY 1024

typedef struct {
    /* Valid advertisements always have a nonzero timestamp, so zero marks an empty slot. */
    advertisement_t *active_slots;
    advertisement_t *flush_slots;
    size_t active_count;
    size_t flush_count;
    uint64_t packets_seen;
    uint64_t packets_dropped;
    portMUX_TYPE lock;
} adv_buffer_state_t;

static advertisement_t s_slot_a[ADV_BUFFER_CAPACITY];
static advertisement_t s_slot_b[ADV_BUFFER_CAPACITY];
static adv_buffer_state_t s_buffer = {
    .active_slots = s_slot_a,
    .flush_slots = s_slot_b,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static size_t hash_mac(const uint8_t mac[6])
{
    uint32_t hash = 2166136261U;

    for (size_t i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619U;
    }
    return hash % ADV_BUFFER_CAPACITY;
}

bool adv_buffer_push(const advertisement_t *adv)
{
    bool stored = false;
    size_t start_idx;

    if (adv == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_buffer.lock);

    s_buffer.packets_seen++;

    start_idx = hash_mac(adv->mac);
    for (size_t i = 0; i < ADV_BUFFER_CAPACITY; i++) {
        size_t idx = (start_idx + i) % ADV_BUFFER_CAPACITY;
        advertisement_t *slot = &s_buffer.active_slots[idx];

        if ((slot->timestamp_us != 0)
            && (memcmp(slot->mac, adv->mac, sizeof(slot->mac)) == 0)) {
            *slot = *adv;
            stored = true;
            break;
        }

        if (slot->timestamp_us == 0) {
            *slot = *adv;
            s_buffer.active_count++;
            stored = true;
            break;
        }
    }

    if (!stored) {
        s_buffer.packets_dropped++;
    }

    portEXIT_CRITICAL(&s_buffer.lock);
    return stored;
}

bool adv_buffer_rotate_window(void)
{
    advertisement_t *tmp_slots;
    bool rotated = false;

    portENTER_CRITICAL(&s_buffer.lock);
    if (s_buffer.flush_count == 0) {
        tmp_slots = s_buffer.flush_slots;
        s_buffer.flush_slots = s_buffer.active_slots;
        s_buffer.active_slots = tmp_slots;
        s_buffer.flush_count = s_buffer.active_count;
        s_buffer.active_count = 0;
        memset(s_buffer.active_slots, 0, sizeof(s_slot_a));
        rotated = true;
    }
    portEXIT_CRITICAL(&s_buffer.lock);

    return rotated;
}

size_t adv_buffer_drain(advertisement_t *out, size_t max_items)
{
    size_t drained = 0;

    if ((out == NULL) || (max_items == 0)) {
        return 0;
    }

    portENTER_CRITICAL(&s_buffer.lock);
    for (size_t i = 0; (i < ADV_BUFFER_CAPACITY) && (drained < max_items)
                       && (s_buffer.flush_count > 0); i++) {
        advertisement_t *slot = &s_buffer.flush_slots[i];
        if (slot->timestamp_us == 0) {
            continue;
        }

        out[drained] = *slot;
        memset(slot, 0, sizeof(*slot));
        s_buffer.flush_count--;
        drained++;
    }
    portEXIT_CRITICAL(&s_buffer.lock);

    return drained;
}

void adv_buffer_get_metrics(adv_buffer_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_buffer.lock);
    memset(metrics, 0, sizeof(*metrics));
    metrics->packets_seen = s_buffer.packets_seen;
    metrics->packets_dropped = s_buffer.packets_dropped;
    metrics->count = (uint32_t) (s_buffer.active_count + s_buffer.flush_count);
    portEXIT_CRITICAL(&s_buffer.lock);
}
