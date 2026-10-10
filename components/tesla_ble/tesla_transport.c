#include "tesla_transport.h"
#include <string.h>

void tesla_stream_reset(tesla_stream_t *s) { memset(s, 0, sizeof(*s)); }

bool tesla_stream_feed(tesla_stream_t *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
    bool ok = true;
    if (s->partial_len && (uint32_t)(now_ms - s->last_rx_ms) > 1000U) s->partial_len = 0;
    s->last_rx_ms = now_ms;
    while (len) {
        size_t target = 2;
        if (s->partial_len >= 2) {
            uint16_t size = ((uint16_t)s->partial[0] << 8) | s->partial[1];
            if (!size || size > TESLA_WIRE_MAX) { s->partial_len = 0; return false; }
            target += size;
        }
        size_t take = target - s->partial_len;
        if (take > len) take = len;
        memcpy(s->partial + s->partial_len, data, take);
        s->partial_len += take;
        data += take; len -= take;
        if (s->partial_len >= 2) {
            uint16_t size = ((uint16_t)s->partial[0] << 8) | s->partial[1];
            if (!size || size > TESLA_WIRE_MAX) { s->partial_len = 0; return false; }
            if (s->partial_len == size + 2) {
                if (s->count < TESLA_INBOX_DEPTH) {
                    uint8_t index = (s->head + s->count) % TESLA_INBOX_DEPTH;
                    memcpy(s->frames[index], s->partial + 2, size);
                    s->lengths[index] = size;
                    ++s->count;
                } else ok = false;
                s->partial_len = 0;
            }
        }
    }
    return ok;
}

bool tesla_stream_pop(tesla_stream_t *s, uint8_t *data, uint16_t *len)
{
    if (!s->count) return false;
    *len = s->lengths[s->head];
    memcpy(data, s->frames[s->head], *len);
    s->head = (s->head + 1) % TESLA_INBOX_DEPTH;
    --s->count;
    return true;
}

bool tesla_replay_accept(tesla_replay_window_t *w, uint32_t c)
{
    if (!w->used) { w->counter = c; w->used = true; return true; }
    if (c == w->counter) return false;
    if (c < w->counter) {
        uint32_t age = w->counter - c;
        if (age > 64 || (w->history & (UINT64_C(1) << (age - 1)))) return false;
        w->history |= UINT64_C(1) << (age - 1);
    } else {
        uint32_t shift = c - w->counter;
        w->history = shift >= 64 ? 0 : w->history << shift;
        if (shift <= 64) w->history |= UINT64_C(1) << (shift - 1);
        w->counter = c;
    }
    return true;
}
