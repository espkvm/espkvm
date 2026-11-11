#pragma once
#include <stddef.h>
#include <stdint.h>
/* Different bytes on every call, so a test can tell a new id from a kept one. */
static inline void esp_fill_random(void *buf, size_t len)
{
    static uint8_t next = 0x5a;
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = next++;
    }
}
