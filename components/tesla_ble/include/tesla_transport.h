#ifndef TESLA_TRANSPORT_H
#define TESLA_TRANSPORT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* vehicle-command: 1024-byte messages, inbox depth 5, 1s partial-frame timeout. */
#define TESLA_WIRE_MAX 1024
#define TESLA_INBOX_DEPTH 5
typedef struct {
    uint8_t partial[TESLA_WIRE_MAX + 2];
    uint16_t partial_len;
    uint32_t last_rx_ms;
    uint8_t frames[TESLA_INBOX_DEPTH][TESLA_WIRE_MAX];
    uint16_t lengths[TESLA_INBOX_DEPTH];
    uint8_t head, count;
} tesla_stream_t;
void tesla_stream_reset(tesla_stream_t *stream);
bool tesla_stream_feed(tesla_stream_t *stream, const uint8_t *data, size_t len, uint32_t now_ms);
bool tesla_stream_pop(tesla_stream_t *stream, uint8_t *data, uint16_t *len);

typedef struct {
    uint32_t counter;
    uint64_t history;
    bool used;
} tesla_replay_window_t;
bool tesla_replay_accept(tesla_replay_window_t *window, uint32_t counter);
#ifdef __cplusplus
}
#endif
#endif
