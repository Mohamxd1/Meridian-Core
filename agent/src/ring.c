#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "meridian.h"

int ring_init(ring_t *r, size_t cap) {
    if (!r || cap == 0) return -1;
    r->items = calloc(cap, sizeof(sample_t));
    if (!r->items) return -1;
    r->cap = cap;
    r->head = r->count = r->dropped = 0;
    return 0;
}

void ring_free(ring_t *r) {
    free(r->items);
    r->items = NULL;
    r->cap = r->head = r->count = 0;
}

int ring_push(ring_t *r, double ts, const char *metric, const char *device, double value) {
    if (!r || !r->items || !metric || !isfinite(value) || !isfinite(ts)) return -1;
    int dropped = 0;
    if (r->count == r->cap) {
        r->head = (r->head + 1) % r->cap;
        r->count--;
        r->dropped++;
        dropped = 1;
    }
    sample_t *s = &r->items[(r->head + r->count) % r->cap];
    s->ts = ts;
    s->value = value;
    strncpy(s->metric, metric, METRIC_LEN - 1);
    s->metric[METRIC_LEN - 1] = '\0';
    strncpy(s->device, device ? device : "", DEVICE_LEN - 1);
    s->device[DEVICE_LEN - 1] = '\0';
    r->count++;
    return dropped;
}

size_t ring_peek(const ring_t *r, sample_t *out, size_t max) {
    size_t n = r->count < max ? r->count : max;
    for (size_t i = 0; i < n; i++) out[i] = r->items[(r->head + i) % r->cap];
    return n;
}

void ring_pop(ring_t *r, size_t n) {
    if (n > r->count) n = r->count;
    r->head = (r->head + n) % r->cap;
    r->count -= n;
}
