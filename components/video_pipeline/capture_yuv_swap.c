/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Putting an LT6911D's bytes in the order the JPEG engine reads.
 *
 * The bridge sends packed 4:2:2 as Y U Y V. The P4's JPEG engine reads the same
 * four bytes the other way round: it takes the two luma from the second and
 * fourth byte and emits them in that order reversed, and the chroma from the
 * first and third. So the four bytes it wants are the four the bridge sends,
 * reversed - which was measured, not read: feeding it Y U Y V gives a flat green
 * picture, U Y V Y gives the right luma with every pair of columns exchanged and
 * red and blue swapped, and only the reversal puts every pixel where the source
 * had it.
 *
 * Nothing else in the chain can do it. The CSI bridge's endian bit reverses a
 * whole 64-bit word, the encoder's own pixel_reverse swaps the two bytes of a
 * 16-bit one, and the colour-mode block that would simply be told the order does
 * not exist below chip revision 3.0.
 *
 * The PPA can: call each 4:2:2 pair one ARGB8888 pixel - it is four bytes either
 * way - and ask for the RGB swap, which reverses them. One pass, no scaling, and
 * the frame keeps its size.
 */
#include "capture_priv.h"

#include "capture_pixfmt.h"

#if CAPTURE_YUV_CODE

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static ppa_client_handle_t s_ppa;
static uint8_t *s_buf;
static size_t s_buf_bytes;

/*
 * Take the buffer before anything else can. Without it the JPEG engine has no
 * picture to encode at all, and the H.264 encoder's reference frame is the one
 * other thing on this board that wants a block this size - whichever asks
 * first wins, and MJPEG is the codec that has to work. Sized for the largest
 * mode, because the pass cannot grow it later once the heap is worked.
 */
esp_err_t capture_yuv_swap_reserve(size_t max_frame_bytes)
{
    if (s_buf_bytes >= max_frame_bytes) {
        return ESP_OK;
    }
    size_t arena_len = 0;
    uint8_t *arena = capture_arena_swap(&arena_len);
    if (arena && arena_len >= max_frame_bytes) {
        s_buf = arena; /* for good: capture_arena.c */
        s_buf_bytes = arena_len;
        return ESP_OK;
    }
    free(s_buf);
    s_buf_bytes = (max_frame_bytes + 63u) & ~(size_t)63u;
    s_buf = heap_caps_aligned_calloc(64, 1, s_buf_bytes, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        s_buf_bytes = 0;
        ESP_LOGE(CAPTURE_LOG_TAG, "no PSRAM for the frame reordering buffer (%u KB)",
                 (unsigned)(max_frame_bytes / 1024u));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

size_t capture_yuv_swap_max_bytes(void)
{
    return (size_t)CAPTURE_MAX_H_RES * CAPTURE_MAX_V_RES * capture_pixfmt()->bpp / 8u;
}

bool capture_yuv_swap_held(void)
{
    return s_buf && s_buf_bytes >= capture_yuv_swap_max_bytes();
}

void capture_yuv_swap_release(void)
{
    if (s_buf && s_buf == capture_arena_swap(NULL)) {
        return; /* in the codec region; nothing to give back */
    }
    free(s_buf);
    s_buf = NULL;
    s_buf_bytes = 0;
}

void *capture_yuv_swap(capture_ctx_t *c, void *src)
{
    if (!s_ppa) {
        ppa_client_config_t cfg = {.oper_type = PPA_OPERATION_SRM};
        if (ppa_register_client(&cfg, &s_ppa) != ESP_OK) {
            s_ppa = NULL;
            return NULL;
        }
    }
    size_t arena_len = 0;
    uint8_t *arena = capture_arena_swap(&arena_len);
    if (s_buf_bytes < c->frame_bytes && arena && arena_len >= c->frame_bytes) {
        s_buf = arena; /* its place in the codec region */
        s_buf_bytes = arena_len;
    } else if (s_buf_bytes < c->frame_bytes) {
        free(s_buf);
        s_buf_bytes = (c->frame_bytes + 63u) & ~(size_t)63u;
        s_buf = heap_caps_aligned_calloc(64, 1, s_buf_bytes, MALLOC_CAP_SPIRAM);
        if (!s_buf) {
            /* Said once per run of failures, not once a frame: without a buffer
             * every frame takes this path and the log would fill at thirty
             * lines a second. */
            static bool said;
            if (!said) {
                said = true;
                ESP_LOGE(CAPTURE_LOG_TAG, "no PSRAM for the frame reordering buffer (%u KB)",
                         (unsigned)(s_buf_bytes / 1024u));
            }
            /* Handing the captured bytes back unchanged would serve a green
             * picture, which is worse than none: say so and let the caller
             * fail. This buffer is taken once and kept for that reason - the
             * H.264 encoder would like the PSRAM, but a working picture in the
             * codec that always works comes first. */
            s_buf_bytes = 0;
            return NULL;
        }
    }

    const int64_t started_us = esp_timer_get_time();
    ppa_srm_oper_config_t srm = {
        .in = {.buffer = src,
               /* four bytes a "pixel": one 4:2:2 pair */
               .pic_w = c->hres / 2u,
               .pic_h = c->vres,
               .block_w = c->hres / 2u,
               .block_h = c->vres,
               .srm_cm = PPA_SRM_COLOR_MODE_ARGB8888},
        .out = {.buffer = s_buf,
                .buffer_size = s_buf_bytes,
                .pic_w = c->hres / 2u,
                .pic_h = c->vres,
                .srm_cm = PPA_SRM_COLOR_MODE_ARGB8888},
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .rgb_swap = true, /* reverses all four bytes of the unit */
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &srm);
    if (err != ESP_OK) {
        static int said;
        if (said < 3) {
            said++;
            ESP_LOGW(CAPTURE_LOG_TAG, "yuv byte swap: %s", esp_err_to_name(err));
        }
        return NULL;
    }

    /* The PPA wrote it by DMA. Drop the lines the allocation left in cache, or
     * the encoder's own writeback puts them back over the frame. */
    (void)esp_cache_msync(s_buf, s_buf_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    capture_status_add_ppa_time((uint32_t)(esp_timer_get_time() - started_us));
    return s_buf;
}

#endif /* CAPTURE_YUV_CODE */

#if CAPTURE_YUV_SWAP
/*
 * The same frame, rearranged for the H.264 encoder instead.
 *
 * Below chip revision 3.0 that encoder takes one layout only: YUV420 with a
 * chroma byte in front of every two luma, U on the even rows and V on the odd
 * ones (esp_h264 calls it O_UYY_E_VYY). Every hardware block that could produce
 * it from YUV422 is revision-gated - the PPA's YUV input modes are shut, and the
 * CSI bridge cannot convert - so this is a plain byte shuffle on the CPU. There
 * is no arithmetic in it: three of every four bytes are copied, and the fourth,
 * the chroma of the odd row, is dropped, which is what makes 4:2:2 into 4:2:0.
 *
 * It replaces the PPA pass the MJPEG path needs rather than adding to it, so
 * H.264 on this board costs about what MJPEG does and a third of the bandwidth.
 *
 * `dst` is the encoder's macroblock-aligned picture; `pad_w` is its width. The
 * rows beyond the real picture are left as they were - the encoder reads them,
 * but nothing looks at what it makes of them.
 */
void capture_yuv422_to_h264(const uint8_t *src, uint8_t *dst, uint32_t hres, uint32_t vres,
                            uint32_t pad_w)
{
    const size_t src_stride = (size_t)hres * 2u;
    const size_t dst_stride = (size_t)pad_w * 3u / 2u;

    for (uint32_t row = 0; row < vres; row++) {
        const uint32_t *in = (const uint32_t *)(const void *)(src + (size_t)row * src_stride);
        uint8_t *o = dst + (size_t)row * dst_stride;
        /*
         * Each source word is Y C Y C, and which chroma it carries alternates:
         * U on the even positions, V on the odd ones. A 4:2:0 row needs one of
         * the two, and which one is not a choice - the encoder wants U on the
         * even rows and V on the odd ones, read out of the PPA's own output for
         * a known colour because nothing documents it. So each row takes the
         * chroma it already carries, half a line from where a careful converter
         * would sample it and invisible at this chroma resolution. Taking both
         * from one row would mean reading two rows for every one written, and
         * this loop is already the slowest thing in the path.
         */
        const unsigned shift = (row & 1u) ? 8u : 24u; /* trying the other way round */
        uint32_t x = 0;
        /* Eight pixels at a time: four words in, three out, all aligned. */
        for (; x + 8u <= hres; x += 8u) {
            const uint32_t a = *in++, b = *in++, c = *in++, d = *in++;
            const uint32_t ca = (a >> shift) & 0xffu, cb = (b >> shift) & 0xffu;
            const uint32_t cc = (c >> shift) & 0xffu, cd = (d >> shift) & 0xffu;
            uint32_t *w = (uint32_t *)(void *)o;
            /* luma reversed inside each pair */
            w[0] = ca | (((a >> 16) & 0xffu) << 8) | ((a & 0xffu) << 16) | (cb << 24);
            w[1] = ((b >> 16) & 0xffu) | ((b & 0xffu) << 8) | (cc << 16) | (((c >> 16) & 0xffu) << 24);
            w[2] = (c & 0xffu) | (cd << 8) | (((d >> 16) & 0xffu) << 16) | ((d & 0xffu) << 24);
            o += 12;
        }
        /* Whatever a width not divisible by eight leaves over. */
        const uint8_t *y = src + (size_t)row * src_stride + (size_t)x * 2u;
        for (; x + 2u <= hres; x += 2u) {
            o[0] = y[(shift == 8u) ? 1 : 3];
            o[1] = y[2];
            o[2] = y[0];
            o += 3;
            y += 4;
        }
    }
}

#endif /* CAPTURE_YUV_SWAP */
