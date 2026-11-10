/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One internal block for the H.264 encoder's reference rows, kept for good.
 *
 * The encoder asks for it on every build: three macroblock rows of the
 * reference, 135 KB at 1920 wide and 90 KB at 1280, in internal RAM only and in
 * one piece. Internal RAM fragments as the device runs. A device that booted at
 * 720p and then saw 1080p found 94 KB as the longest run and fell back to MJPEG
 * until a restart (Function EV, 2026-10-24).
 *
 * So the first request takes a block sized for the widest picture, and every
 * later build gets that same block back. The component frees it with
 * heap_caps_free() (esp_h264_free is a macro for it), so both ends are wrapped
 * at link time; the component has no allocator hook to use instead. At 1080p
 * this holds what the encoder held anyway; at 720p 45 KB more.
 */
#include "capture_priv.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* The component's own sizing (max_refame_buffer_size in esp_h264_enc_hw_param.c):
 * 3 rows x 16 lines x (16 Y + 8 UV) bytes per macroblock, plus 7 for alignment. */
#define REF_BYTES_PER_MB (3u * 16u * (16u + 8u))
#define REF_ALIGN_PAD 7u
#define REF_POOL_BYTES (REF_BYTES_PER_MB * ((CAPTURE_MAX_H_RES + 15u) / 16u) + REF_ALIGN_PAD)
/* The reference is the only 16-byte-aligned internal request this big; the
 * descriptors are tiny and the deblocking buffer asks with alignment 4. */
#define REF_MIN_BYTES (32u * 1024u)

void *__real_esp_h264_aligned_malloc(uint32_t alignment, uint32_t n, uint32_t size,
                                     uint32_t *actual_size, uint32_t caps);
void __real_heap_caps_free(void *ptr);

static void *s_pool;
static uint32_t s_pool_len;
static bool s_pool_used;
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;

void *__wrap_esp_h264_aligned_malloc(uint32_t alignment, uint32_t n, uint32_t size,
                                     uint32_t *actual_size, uint32_t caps)
{
    const uint32_t bytes = n * size;
    const bool is_ref = alignment == 16u && bytes >= REF_MIN_BYTES &&
                        (caps & MALLOC_CAP_INTERNAL) && !(caps & MALLOC_CAP_SPIRAM);
    if (!is_ref) {
        return __real_esp_h264_aligned_malloc(alignment, n, size, actual_size, caps);
    }
    if (!s_pool) {
        const uint32_t want = bytes > REF_POOL_BYTES ? bytes : REF_POOL_BYTES;
        s_pool = __real_esp_h264_aligned_malloc(alignment, 1, want, &s_pool_len, caps);
        if (s_pool) {
            ESP_LOGI(CAPTURE_LOG_TAG, "h264 reference rows: %u KB internal, kept",
                     (unsigned)(s_pool_len / 1024u));
        }
    }
    bool take = false;
    portENTER_CRITICAL(&s_mu);
    if (s_pool && !s_pool_used && bytes <= s_pool_len) {
        s_pool_used = true;
        take = true;
    }
    portEXIT_CRITICAL(&s_mu);
    if (take) {
        *actual_size = s_pool_len;
        return s_pool;
    }
    /* Wider than the widest picture, or a second encoder: the plain way. */
    return __real_esp_h264_aligned_malloc(alignment, n, size, actual_size, caps);
}

bool h264_ref_pool_ready(void)
{
    return s_pool && s_pool_len >= REF_POOL_BYTES;
}

/* In IRAM like heap_caps_free itself: it is called with the flash cache off. */
void IRAM_ATTR __wrap_heap_caps_free(void *ptr)
{
    if (ptr && ptr == s_pool) {
        portENTER_CRITICAL_SAFE(&s_mu);
        s_pool_used = false;
        portEXIT_CRITICAL_SAFE(&s_mu);
        return;
    }
    __real_heap_caps_free(ptr);
}
