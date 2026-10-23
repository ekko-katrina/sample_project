#ifndef APP_REMOTE_FRAME_H
#define APP_REMOTE_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int remote_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline size_t remote_decode_frame(const char *hex, uint8_t *out, size_t capacity)
{
    if (hex == NULL) return 0;
    size_t n = strlen(hex);
    if (n % 2U != 0U || n < 12U || n / 2U > capacity) return 0;
    for (size_t i = 0; i < n / 2U; ++i) {
        int high = remote_hex_digit(hex[i * 2U]);
        int low = remote_hex_digit(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) return 0;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return n / 2U;
}
#endif
