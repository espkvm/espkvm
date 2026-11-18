/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One PSRAM region for the codecs, taken at boot and never given back.
 *
 * Each codec used to allocate and free its own buffers: MJPEG three outputs of
 * 2.4 MB, H.264 two YUV buffers of 3 MB and two outputs of 1 MB, plus the 4 MB
 * byte-reordering buffer on the boards that need one. Switching codec gave the
 * space back to the heap and asked for it again in other sizes, and after a
 * few switches the heap came back in pieces: 17 MB free, largest block 4032 KB,
 * and a 4050 KB buffer that would not fit (M5Stack at 1080p, 2026-09-24).
 *
 * Here the space is one block for good. The codec that runs carves it from
 * offset 0; the other one's buffers are simply not in use. While H.264 runs,
 * the part it does not need is lent to the recorder for its ring, and taken
 * back before MJPEG opens.
 *
 * Every piece starts on and is sized to 64 bytes: the JPEG, H.264 and PPA
 * drivers write back and invalidate whole cache lines of their buffers.
 */
#include "capture_priv.h"

#include <stdint.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define ARENA_ALIGN 64u

static uint8_t *s_base;
static size_t s_swap_len;  /* at the start, for good; 0 without the reorder pass */
static size_t s_codec_len; /* after it */
static size_t s_used;      /* by the codec that last claimed the region */
static bool s_lent;        /* the tail past s_used is the recorder's */
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;

size_t capture_arena_round(size_t bytes)
{
    return (bytes + ARENA_ALIGN - 1u) & ~(size_t)(ARENA_ALIGN - 1u);
}

esp_err_t capture_arena_init(void)
{
    if (s_base) {
        return ESP_OK;
    }
    size_t swap = 0;
#if CAPTURE_YUV_SWAP
    swap = capture_arena_round(capture_yuv_swap_max_bytes());
#endif
    const size_t mjpeg = capture_mjpeg_arena_bytes();
    const size_t h264 = capture_h264_arena_bytes();
    const size_t codec = mjpeg > h264 ? mjpeg : h264;
    s_base = heap_caps_aligned_calloc(ARENA_ALIGN, 1, swap + codec, MALLOC_CAP_SPIRAM);
    if (!s_base) {
        ESP_LOGE(CAPTURE_LOG_TAG, "no PSRAM for the codec region (%u KB); codecs allocate their own",
                 (unsigned)((swap + codec) / 1024u));
        return ESP_ERR_NO_MEM;
    }
    /* calloc zeroed it through the cache; nothing dirty may land on DMA data later. */
    (void)esp_cache_msync(s_base, swap + codec, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_swap_len = swap;
    s_codec_len = codec;
    ESP_LOGI(CAPTURE_LOG_TAG, "codec region: %u KB (reorder %u, MJPEG %u, H.264 %u)",
             (unsigned)((swap + codec) / 1024u), (unsigned)(swap / 1024u),
             (unsigned)(mjpeg / 1024u), (unsigned)(h264 / 1024u));
    return ESP_OK;
}

bool capture_arena_active(void)
{
    return s_base != NULL;
}

uint8_t *capture_arena_swap(size_t *len)
{
    if (!s_base || !s_swap_len) {
        return NULL;
    }
    if (len) {
        *len = s_swap_len;
    }
    return s_base;
}

uint8_t *capture_arena_claim(size_t bytes)
{
    if (!s_base || bytes > s_codec_len) {
        return NULL;
    }
    bytes = capture_arena_round(bytes);
    bool ok;
    taskENTER_CRITICAL(&s_mu);
    ok = !s_lent || bytes <= s_used;
    if (ok) {
        s_used = bytes;
    }
    taskEXIT_CRITICAL(&s_mu);
    if (!ok) {
        return NULL; /* the recorder still has the tail; capture_release_memory() */
    }
    uint8_t *codec = s_base + s_swap_len;
    /* The other codec wrote here through the cache; flush and drop those lines
     * before this one's DMA writes the same addresses. */
    (void)esp_cache_msync(codec, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    return codec;
}

void *capture_arena_borrow(size_t min, size_t *len)
{
    void *p = NULL;
    taskENTER_CRITICAL(&s_mu);
    if (s_base && !s_lent && s_used && s_codec_len - s_used >= min) {
        s_lent = true;
        p = s_base + s_swap_len + s_used;
        *len = s_codec_len - s_used;
    }
    taskEXIT_CRITICAL(&s_mu);
    return p;
}

void capture_arena_give_back(void *p)
{
    if (!p) {
        return;
    }
    taskENTER_CRITICAL(&s_mu);
    s_lent = false;
    taskEXIT_CRITICAL(&s_mu);
}
