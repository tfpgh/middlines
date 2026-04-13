#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "advertisement.h"

typedef struct {
    uint64_t packets_seen;
    uint64_t packets_enqueued;
    uint64_t packets_dropped;
    uint32_t high_watermark;
    uint32_t count;
    uint32_t capacity;
} adv_buffer_metrics_t;

bool adv_buffer_is_initialized(void);
bool adv_buffer_init(size_t capacity);
bool adv_buffer_push(const advertisement_t *adv);
bool adv_buffer_rotate_window(void);
size_t adv_buffer_drain(advertisement_t *out, size_t max_items);
void adv_buffer_get_metrics(adv_buffer_metrics_t *metrics);
