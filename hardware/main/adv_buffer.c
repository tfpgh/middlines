#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "adv_buffer.h"

typedef struct {
    bool occupied;
    advertisement_t adv;
} adv_slot_t;

typedef struct {
    adv_slot_t *active_slots;
    adv_slot_t *flush_slots;
    size_t capacity;
    size_t active_count;
    size_t flush_count;
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
    return (s_buffer.active_slots != NULL) && (s_buffer.flush_slots != NULL);
}

bool adv_buffer_init(size_t capacity)
{
    if (adv_buffer_is_initialized()) {
        return true;
    }

    s_buffer.active_slots = calloc(capacity, sizeof(adv_slot_t));
    if (s_buffer.active_slots == NULL) {
        return false;
    }

    s_buffer.flush_slots = calloc(capacity, sizeof(adv_slot_t));
    if (s_buffer.flush_slots == NULL) {
        free(s_buffer.active_slots);
        s_buffer.active_slots = NULL;
        return false;
    }

    s_buffer.capacity = capacity;
    return true;
}

static size_t hash_mac(uint64_t mac, size_t capacity)
{
    mac ^= mac >> 33;
    mac *= 0xff51afd7ed558ccdULL;
    mac ^= mac >> 33;
    return (size_t) (mac % capacity);
}

bool adv_buffer_push(const advertisement_t *adv)
{
    bool stored = false;
    size_t start_idx;

    if (!adv_buffer_is_initialized() || (adv == NULL)) {
        return false;
    }

    portENTER_CRITICAL(&s_buffer.lock);

    s_buffer.packets_seen++;

    start_idx = hash_mac(adv->mac, s_buffer.capacity);
    for (size_t i = 0; i < s_buffer.capacity; i++) {
        size_t idx = (start_idx + i) % s_buffer.capacity;
        adv_slot_t *slot = &s_buffer.active_slots[idx];

        if (slot->occupied && (slot->adv.mac == adv->mac)) {
            slot->adv = *adv;
            stored = true;
            break;
        }

        if (!slot->occupied) {
            slot->occupied = true;
            slot->adv = *adv;
            s_buffer.active_count++;
            s_buffer.packets_enqueued++;
            if ((s_buffer.active_count + s_buffer.flush_count) > s_buffer.high_watermark) {
                s_buffer.high_watermark = (uint32_t) (s_buffer.active_count + s_buffer.flush_count);
            }
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
    adv_slot_t *tmp_slots;
    bool rotated = false;

    if (!adv_buffer_is_initialized()) {
        return false;
    }

    portENTER_CRITICAL(&s_buffer.lock);
    if (s_buffer.flush_count == 0) {
        tmp_slots = s_buffer.flush_slots;
        s_buffer.flush_slots = s_buffer.active_slots;
        s_buffer.active_slots = tmp_slots;
        s_buffer.flush_count = s_buffer.active_count;
        s_buffer.active_count = 0;
        memset(s_buffer.active_slots, 0, s_buffer.capacity * sizeof(s_buffer.active_slots[0]));
        rotated = true;
    }
    portEXIT_CRITICAL(&s_buffer.lock);

    return rotated;
}

size_t adv_buffer_drain(advertisement_t *out, size_t max_items)
{
    size_t drained = 0;

    if (!adv_buffer_is_initialized() || (out == NULL) || (max_items == 0)) {
        return 0;
    }

    portENTER_CRITICAL(&s_buffer.lock);
    for (size_t i = 0; (i < s_buffer.capacity) && (drained < max_items) && (s_buffer.flush_count > 0); i++) {
        adv_slot_t *slot = &s_buffer.flush_slots[i];
        if (!slot->occupied) {
            continue;
        }

        out[drained] = slot->adv;
        slot->occupied = false;
        memset(&slot->adv, 0, sizeof(slot->adv));
        s_buffer.flush_count--;
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
    metrics->count = (uint32_t) (s_buffer.active_count + s_buffer.flush_count);
    metrics->capacity = (uint32_t) s_buffer.capacity;
    portEXIT_CRITICAL(&s_buffer.lock);
}
