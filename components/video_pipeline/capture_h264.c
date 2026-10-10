/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * H.264: CSI (RGB888) -> PPA (YUV420) -> hardware encoder -> Annex-B.
 *
 * The trade this makes is described in h264_probe.c: it is slower per frame
 * than MJPEG because of the colour conversion, and far cheaper on the wire
 * because a screen that barely changes produces P-frames of a few hundred
 * bytes instead of another 82 KB JPEG. On a KVM, which shows a mostly static
 * screen, the wire wins.
 *
 * Two things the encoder decides for us, both visible here:
 *  - SPS and PPS are emitted in front of every IDR, so a viewer that joins
 *    mid-stream needs no side-channel to configure its decoder - only an IDR,
 *    which is what video_frame_request_keyframe() asks for.
 *  - Frame size is fixed at open, so a resolution change means a new encoder.
 */
#include "h264_sps.h"
#include "capture_priv.h"

#include <inttypes.h>
#include <stdint.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "video_frame.h"

/** Macroblocks are 16x16, so the encoder works on a size rounded up to that.
 *  1080 becomes 1088; the SPS crops the padding away again. */
#define MB_ALIGN(v) (((v) + 15u) & ~15u)

#define H264_MAX_W MB_ALIGN(CAPTURE_MAX_H_RES)
#define H264_MAX_H MB_ALIGN(CAPTURE_MAX_V_RES)

/*
 * rev >= 3.0 lets the H.264 encoder consume the captured pixels directly (BGR888
 * today, YUV422 next), dropping the PPA colour-convert pass and its ~9 MB/frame of
 * PSRAM traffic. The rev < 3.0 encoder accepts only O_UYY_E_VYY, so that build
 * keeps the PPA path. The fork is gated on CAPTURE_DIRECT_ENCODE (capture_pixfmt.h)
 * so the rev < 3.0 binary never compiles the direct path; the encoder input format
 * itself comes from the active pixel profile. */

/**
 * Two output slots, not three: unlike MJPEG these are small, and a second slot
 * is already enough for an encode to proceed while a viewer sends the previous
 * frame.
 */
#define H264_SLOTS 2

/** Room for the largest frame worth sending; an IDR at 1080p is far below it. */
#define H264_SLOT_CAP ((size_t)CAPTURE_MAX_H_RES * CAPTURE_MAX_V_RES / 2u)

static esp_h264_enc_handle_t s_enc;
static esp_h264_enc_param_hw_handle_t s_param;
/** Whether the encoder currently holds the H.264 block, its interrupt and DMA.
 *  It stays true across a codec switch: the encoder keeps both, see
 *  encoder_park(). */
static bool s_enc_hw_held;
static uint8_t *s_buf[H264_SLOTS];
static uint32_t s_buf_alloc[H264_SLOTS];

#if !CAPTURE_DIRECT_ENCODE
/*
 * rev < 3.0 path only. PPA colour conversion and the encode run in two tasks over
 * two YUV buffers, so one frame is being encoded while the next is being converted.
 * Measured on this board the conversion dominates (~104 ms vs ~45 ms at 1080p), so
 * overlapping lifts the frame rate by roughly the encode time. The capture loop runs
 * the PPA stage (it must consume the CSI frame promptly, before the receiver reuses
 * it); a dedicated task runs the encoder from whichever YUV buffer the PPA just
 * filled. A free-slot queue and a job queue pass the two buffers between them.
 *
 * The direct-encode path (rev >= 3.0) feeds the captured pixels to the encoder
 * synchronously and allocates none of this - no PPA client, no intermediate YUV
 * buffers, no second task - freeing ~6 MB of PSRAM for a third capture buffer.
 */
static ppa_client_handle_t s_ppa;
#define H264_YUV_BUFS 2
static uint8_t *s_yuv[H264_YUV_BUFS];
static uint32_t s_yuv_alloc[H264_YUV_BUFS];


typedef struct {
    int slot; /* index into s_yuv, or -1 as the shutdown sentinel */
    uint32_t hres;
    uint32_t vres;
} h264_job_t;

static QueueHandle_t s_free_slots; /* YUV buffers the PPA may write */
static QueueHandle_t s_jobs;       /* YUV buffers filled and awaiting encode */
static SemaphoreHandle_t s_enc_done;
static TaskHandle_t s_enc_task;

static void h264_encode_task(void *arg);
#endif

/** Size the encoder is currently configured for, 0 when it is not open. */
static uint32_t s_enc_w;
static uint32_t s_enc_h;
/**
 * The encoder could not be built and retrying will not help.
 *
 * Set when the hardware encoder cannot get its memory, which is a state the
 * pipeline cannot encode its way out of: the size it needs is fixed by the
 * input resolution. Without this the encode path retried on every captured
 * frame - some thirty times a second, forever, each one logging the same
 * error - while the viewer sat looking at nothing.
 */
static bool s_enc_broken;

/*
 * The rate controller can settle at its coarsest quantiser and stay there.
 *
 * Seen on a device left running: the target sat on a still screen, the stream
 * fell to 11 kbit/s of the 4000 it was allowed, and the picture went to visible
 * blocks for minutes at a time - keyframes of 6.7 KB at QP 40 where the same
 * screen had been 148 KB at QP 13 a minute earlier. It swings back on its own,
 * then wedges again. Nothing that can be set on a running encoder moves it:
 * writing the bitrate (even a much larger one), lengthening the GOP, asking for
 * keyframes, parking it across a codec switch. What does clear it is building a
 * new encoder - proved by changing the source's resolution, which is the one
 * thing that makes this file do that.
 *
 * So watch the keyframes. A screen that has not changed produces keyframes of
 * about the same size; one that collapses by this much, this many times in a
 * row, is the controller stuck rather than the picture getting simpler.
 * REF_MIN keeps a genuinely plain screen - a black one, a text console - out of
 * it: those never reach the reference size in the first place. After a rebuild
 * the reference starts again from the new encoder's own output, so a screen
 * that really is that cheap settles at its own size and never trips this twice.
 */
#define WEDGE_RATIO 8u              /* keyframe this many times below the best */
#define WEDGE_REF_MIN (64u * 1024u) /* ... of a reference at least this big */
#define WEDGE_IDRS 3u               /* consecutive starved keyframes */
#define WEDGE_COOLDOWN_US (120 * 1000 * 1000)

static uint32_t s_idr_best;
static uint32_t s_idr_starved;
static bool s_wedged;
static int64_t s_rebuild_after_us;

/*
 * A keyframe at least this often, whatever the frame rate. The GOP is counted in
 * frames for the configured rate, so a pre-3.0 board encoding 1080p at 7 fps
 * would otherwise send one every nine seconds - too far apart for the recorder's
 * ring and a timelapse, and for a viewer joining mid-stream.
 */
#define KEYFRAME_MAX_US 2500000
static int64_t s_last_idr_us;

/** Called for every frame the encoder returns; only keyframes carry the signal. */
static void wedge_watch(bool is_idr, uint32_t len)
{
    if (is_idr) {
        s_last_idr_us = esp_timer_get_time();
    }
    if (!is_idr || len == 0) {
        return;
    }
    if (len >= s_idr_best) {
        s_idr_best = len;
        s_idr_starved = 0;
        return;
    }
    if (s_idr_best >= WEDGE_REF_MIN && (uint64_t)len * WEDGE_RATIO < s_idr_best) {
        if (++s_idr_starved >= WEDGE_IDRS) {
            s_wedged = true;
        }
    } else {
        s_idr_starved = 0;
    }
}

bool capture_h264_encoder_failed(void)
{
    return s_enc_broken;
}

static esp_err_t encoder_open(uint32_t w, uint32_t h);

/*
 * Take the encoder's internal RAM at boot, before the network cuts it up.
 *
 * The reference frame is one internal block of 135 KB at 1920 wide. On a
 * pre-3.0 P4-ETH the longest run was 139 KB when capture started and 132 KB on
 * the next boot, and 86 KB a minute after a browser connected. So a device that
 * booted on MJPEG could not switch to H.264. Built before the network and
 * parked, the switch takes it back.
 */
void capture_h264_reserve(uint32_t w, uint32_t h)
{
    if (s_enc || !kvm_cap_available(KVM_CAP_H264)) {
        return;
    }
    if (w == 0 || h == 0) {
        w = CAPTURE_MAX_H_RES;
        h = CAPTURE_MAX_V_RES;
    }
    if (encoder_open(w, h) != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "h264 encoder not reserved; it will be built on first use");
    }
}

/** Last values pushed into the encoder, so a setting change is noticed. */
static uint8_t s_gop;
static uint32_t s_bitrate;

/* Shared with h264_probe.c (same component); forward-declared there. */
const char *h264_err_name(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return "ok";
    case ESP_H264_ERR_ARG:
        return "invalid argument";
    case ESP_H264_ERR_MEM:
        return "out of memory";
    case ESP_H264_ERR_UNSUPPORTED:
        return "unsupported on this chip";
    case ESP_H264_ERR_TIMEOUT:
        return "timeout";
    case ESP_H264_ERR_OVERFLOW:
        return "output buffer overflow";
    default:
        return "failed";
    }
}

/* What a working encoder took from internal RAM, measured on the open that
 * succeeded. A rebuild checks the longest free run against it. */
static size_t s_enc_internal_bytes;

/*
 * Reserving the reference frame's block does not work on this chip, tried three
 * ways on hardware 2026-09-23 and each one measured.
 *
 * The encoder wants one contiguous internal block - 151 KB at 1080p, 101 at
 * 720p - and internal RAM fragments as the device runs, so a rebuild after a
 * resolution change finds the longest run at 132 KB and H.264 is gone until a
 * restart. Holding that block from the side does not save it: a round 160 KB
 * can never be taken, because the space an encoder frees is exactly its own
 * size; a smaller stand-in sits in the middle of that space and stops it
 * merging; and holding the whole 151 KB permanently starves TLS - the device
 * answered HTTP and reset every HTTPS handshake, which is the web interface
 * gone. Internal RAM is not ours to hoard.
 *
 * What is left is what happens now: the switch falls back to MJPEG and says
 * why, in both heaps' numbers. A device that needs H.264 after changing
 * resolution has to be restarted.
 */

static void encoder_release(void)
{
    if (s_enc) {
        if (s_enc_hw_held) {
            esp_h264_enc_close(s_enc);
        }
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
    }
    s_enc_hw_held = false;
    s_param = NULL;
    s_enc_w = 0;
    s_enc_h = 0;
    /* The keyframe reference belongs to the instance that produced it. */
    s_idr_best = 0;
    s_idr_starved = 0;
    s_wedged = false;
}

/*
 * Keep the encoder whole across a codec switch.
 *
 * Building it needs one CONTIGUOUS internal block for the reference frame -
 * 135 KB at 1080p, 90 KB at 720p - and the component asks for internal only,
 * with no fall back to PSRAM. Internal RAM is what this chip has least of, and
 * it fragments as the device runs. A device that had H.264 open, spent a while
 * on MJPEG and then came back could not get that block again: seen on the bench
 * at 323 KB free with the longest run only 132 KB, after which the codec
 * silently stayed on MJPEG.
 *
 * Closing the encoder is not a way out. esp_h264_enc_close() resets the H.264
 * block and deinits its DMA, while esp_h264_enc_open() only takes the interrupt
 * back: the DMA and its descriptors are set up in esp_h264_enc_hw_new(). A
 * closed-then-opened encoder drives a dead DMA and takes the device down with
 * it, which is what the bench showed - every switch back rebooted the board.
 *
 * So a codec switch leaves the encoder alone. It costs the memory we were using
 * anyway while H.264 ran, plus an idle interrupt, and the switch back cannot
 * fail. The buffers are freed for real when the resolution changes, which is
 * the one case they are the wrong size.
 */
static void encoder_park(void)
{
    /* Deliberately nothing: the point is to keep both memory and hardware. */
}

/** Keyframe interval: two seconds of the current frame rate, within the byte
 *  the encoder takes. Long enough not to cost bandwidth, short enough that a
 *  decoder recovers on its own if it ever loses sync. */
static uint8_t wanted_gop(void)
{
    int32_t fps = kvm_setting_int("vid_fps_max");
    if (fps < 1) {
        fps = 30;
    }
    int32_t gop = fps * 2;
    if (gop > 250) {
        gop = 250;
    }
    return (uint8_t)gop;
}

static uint32_t wanted_bitrate(void)
{
    int32_t kbps = kvm_setting_int("h264_kbps");
    if (kbps < 100) {
        kbps = 4000;
    }
    /* Less while a viewer's link is behind; see capture_link_pct(). */
    kbps = kbps * (int32_t)capture_link_pct() / 100;
    if (kbps < 300) {
        kbps = 300;
    }
    return (uint32_t)kbps * 1000u;
}

/*
 * How fine the encoder may get, and how coarse it may fall back to.
 *
 * qp_min is a quality ceiling, not a bandwidth one: the rate controller already
 * holds the stream to the configured bitrate, and this only says how good a
 * frame is allowed to be when there is budget left over.
 *
 * qp_max is the one that decides what a still screen looks like, and 45 was far
 * too coarse. Measured on a device left overnight: the target showed a dark
 * screensaver, the stream sat at 11 kbit/s of the 4000 it was allowed, and the
 * picture was in visible blocks - for as long as nothing moved. The rate
 * controller had no reason to spend more: the frames were "cheap" and it parked
 * at the coarsest quantisation it was permitted. Raising the budget to 12 Mbit/s
 * changed neither the bitrate nor the picture, which is what proved it was the
 * ceiling and not the bandwidth. Once nothing moves, nothing refines it either:
 * the P-frames say "no change", so the blocks stay until the screen does
 * something.
 *
 * 32 is coarse enough to leave the controller room on a busy screen and fine
 * enough that a still one does not break into squares. An earlier report of the
 * same symptom was put down to the source losing its HDMI link every few
 * seconds; that was a different fault, and this was the rest of it.
 *
 * The pair is fixed when the encoder is built - the component has setters for
 * fps, GOP and bitrate, and none for these - so a change here only reaches a
 * newly built encoder, not one taken back from the parked slot. In practice
 * that means a restart.
 */
#define H264_QP_MIN 18
#define H264_QP_MAX 32

static esp_err_t encoder_open(uint32_t w, uint32_t h)
{
    int32_t fps = kvm_setting_int("vid_fps_max");
    if (fps < 1 || fps > 255) {
        fps = 30;
    }
    s_gop = wanted_gop();
    s_bitrate = wanted_bitrate();

    /* A parked encoder of the right size is taken back rather than rebuilt: it
     * still holds the internal block that a rebuild might not find. Settings
     * that changed while it was parked are applied here, and the first frame
     * out is an IDR because opening resets the frame counter. */
    if (s_enc && s_enc_hw_held && s_enc_w == w && s_enc_h == h) {
        (void)esp_h264_enc_hw_get_param_hd(s_enc, &s_param);
        if (s_param) {
            (void)esp_h264_enc_set_fps(&s_param->base, (uint8_t)fps);
            (void)esp_h264_enc_set_gop(&s_param->base, s_gop);
            (void)esp_h264_enc_set_bitrate(&s_param->base, s_bitrate);
        }
        s_enc_broken = false;
        ESP_LOGI(CAPTURE_LOG_TAG,
                 "h264 encoder %" PRIu32 "x%" PRIu32 " taken back @%" PRId32 " fps, %" PRIu32
                 " kbit/s, gop %u", w, h, fps, s_bitrate / 1000u, s_gop);
        return ESP_OK;
    }

    /* Wrong size, or nothing parked: build one. */
    encoder_release();

    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = (esp_h264_raw_format_t)capture_pixfmt()->h264_pic,
        .gop = s_gop,
        .fps = (uint8_t)fps,
        .res = {.width = (uint16_t)w, .height = (uint16_t)h},
        .rc = {.bitrate = s_bitrate, .qp_min = H264_QP_MIN, .qp_max = H264_QP_MAX},
    };
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_h264_err_t herr = esp_h264_enc_hw_new(&cfg, &s_enc);
    if (herr != ESP_H264_ERR_OK || !s_enc) {
        /*
         * The reference frame wants one long run of PSRAM, and the recorder's
         * ring of frames is usually what breaks the longest one in two. Ask for
         * it back and build the encoder again before giving up on H.264 - the
         * dashcam can wait, a picture cannot.
         */
        if (capture_release_memory(true)) {
            ESP_LOGW(CAPTURE_LOG_TAG, "h264 encoder: no memory; asked the recorder for its buffer");
            vTaskDelay(pdMS_TO_TICKS(200));
            herr = esp_h264_enc_hw_new(&cfg, &s_enc);
        }
    }
    if (herr != ESP_H264_ERR_OK || !s_enc) {
        /*
         * Print BOTH heaps, and internal first: the encoder takes its working
         * buffers from internal memory, of which this chip has about half a
         * megabyte in total, while PSRAM sits there with tens of megabytes
         * free. Logging PSRAM alone - which this did at first - reads as "the
         * device is out of memory" and sends everyone off measuring the wrong
         * heap. Largest block as well as free: a single multi-megabyte request
         * fails on the longest free run, not on the total.
         */
        ESP_LOGE(CAPTURE_LOG_TAG,
                 "h264 encoder for %" PRIu32 "x%" PRIu32 ": %s (internal %u KB free / %u KB "
                 "largest, PSRAM %u KB free / %u KB largest)",
                 w, h, h264_err_name(herr),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        s_enc = NULL;
        /* Distinct from every other failure: the caller drops to MJPEG on this
         * one rather than retrying a frame later, forever. */
        return herr == ESP_H264_ERR_MEM ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }
    herr = esp_h264_enc_open(s_enc);
    if (herr != ESP_H264_ERR_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 encoder open: %s", h264_err_name(herr));
        encoder_release();
        return ESP_FAIL;
    }
    (void)esp_h264_enc_hw_get_param_hd(s_enc, &s_param);
    s_enc_hw_held = true;
    s_enc_w = w;
    s_enc_h = h;
    s_enc_broken = false;
    /*
     * What it cost in internal RAM, so a later rebuild can refuse to start when
     * that much is not there to be had. The reference frame alone is one
     * contiguous internal block - about 135 KB at 1920 wide - and the component
     * has no PSRAM fallback for it.
     */
    const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_enc_internal_bytes = internal_before > internal_after ? internal_before - internal_after : 0;
    ESP_LOGI(CAPTURE_LOG_TAG, "h264 encoder %" PRIu32 "x%" PRIu32 " @%" PRId32 " fps, %" PRIu32
             " kbit/s, gop %u, %u KB internal", w, h, fps, s_bitrate / 1000u, s_gop,
             (unsigned)(s_enc_internal_bytes / 1024));
    return ESP_OK;
}

/**
 * Build a new encoder when the old one has wedged, at most once every two
 * minutes. Runs where encoder_open() is already legal - the top of an encode,
 * before a frame is handed over.
 *
 * @return true when the caller should give up on this frame (the rebuild
 *         failed, and s_enc_broken now sends the stream to MJPEG).
 */
static bool wedge_rebuild_if_needed(uint32_t w, uint32_t h)
{
    if (!s_wedged) {
        return false;
    }
    /* A settings switch, because this judges a picture by its size and could in
     * principle read some screen wrong. Off means the blocks stay. */
    if (!kvm_setting_bool("h264_guard")) {
        s_wedged = false;
        s_idr_starved = 0;
        return false;
    }
    const int64_t now = esp_timer_get_time();
    if (now < s_rebuild_after_us) {
        return false;
    }
    /*
     * Only rebuild when the room for a new encoder is already free, without
     * counting on the old one giving its block back. It does not always: the
     * reference frame wants one contiguous internal run, and releasing a 135 KB
     * block does not reliably leave a 135 KB hole. A rebuild that fails here
     * costs the whole codec - the caller drops to MJPEG until the next restart -
     * so a picture in blocks is the better of the two. Seen on the bench with
     * 314 KB free and the longest run 132 KB.
     */
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (s_enc_internal_bytes && largest < s_enc_internal_bytes) {
        ESP_LOGW(CAPTURE_LOG_TAG,
                 "keyframes fell, but a new encoder needs %u KB of internal RAM in one piece and "
                 "the longest free run is %u KB - leaving it alone",
                 (unsigned)(s_enc_internal_bytes / 1024), (unsigned)(largest / 1024));
        s_wedged = false;
        s_idr_starved = 0;
        s_rebuild_after_us = now + WEDGE_COOLDOWN_US;
        return false;
    }
    ESP_LOGW(CAPTURE_LOG_TAG,
             "keyframes fell to %" PRIu32 " bytes from %" PRIu32 " - rebuilding the encoder",
             s_idr_best / WEDGE_RATIO, s_idr_best);
    s_wedged = false;
    s_idr_starved = 0;
    s_idr_best = 0;
    s_rebuild_after_us = now + WEDGE_COOLDOWN_US;
    encoder_release();
    if (encoder_open(w, h) != ESP_OK) {
        s_enc_broken = true;
        return true;
    }
    (void)video_frame_take_keyframe_request(); /* a fresh encoder starts on an IDR */
    return false;
}

static size_t h264_slot_bytes(void)
{
    return capture_arena_round(H264_SLOT_CAP);
}

#if !CAPTURE_DIRECT_ENCODE
static size_t h264_yuv_bytes(void)
{
    return capture_arena_round((size_t)H264_MAX_W * H264_MAX_H * 3u / 2u);
}
#endif

size_t capture_h264_arena_bytes(void)
{
    size_t bytes = H264_SLOTS * h264_slot_bytes();
#if !CAPTURE_DIRECT_ENCODE
    bytes += H264_YUV_BUFS * h264_yuv_bytes();
#endif
    return bytes;
}

static void h264_free_buffers(void)
{
    /* Pieces of the codec region are never freed, only let go of. */
    const bool own = !capture_arena_active();
#if !CAPTURE_DIRECT_ENCODE
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        if (s_yuv[i]) {
            if (own) {
                esp_h264_free(s_yuv[i]);
            }
            s_yuv[i] = NULL;
        }
    }
#endif
    for (int i = 0; i < H264_SLOTS; i++) {
        if (s_buf[i]) {
            if (own) {
                esp_h264_free(s_buf[i]);
            }
            s_buf[i] = NULL;
        }
    }
}

/* The buffers out of the codec region; false when the recorder still has it. */
static bool h264_take_arena(void)
{
    uint8_t *base = capture_arena_claim(capture_h264_arena_bytes());
    if (!base) {
        return false;
    }
    const size_t slot = h264_slot_bytes();
    for (int i = 0; i < H264_SLOTS; i++) {
        s_buf[i] = base + (size_t)i * slot;
        s_buf_alloc[i] = (uint32_t)slot;
    }
#if !CAPTURE_DIRECT_ENCODE
    uint8_t *yuv = base + H264_SLOTS * slot;
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        s_yuv[i] = yuv + (size_t)i * h264_yuv_bytes();
        s_yuv_alloc[i] = (uint32_t)h264_yuv_bytes();
    }
#endif
    return true;
}

/*
 * MJPEG keeps its output buffers across a close so that it can always come
 * back (see capture_mjpeg.c). They are 4.8 MB together, which on a tight board
 * is the difference between this encoder starting and not, so a failure here
 * asks for them once and tries again.
 */
static esp_err_t h264_open_once(void);

static esp_err_t h264_open(void)
{
    esp_err_t err = h264_open_once();
    if (err == ESP_ERR_NO_MEM && capture_arena_active() && capture_release_memory(true)) {
        err = h264_open_once(); /* the recorder had the region's tail */
    }
    if (err == ESP_ERR_NO_MEM && !capture_arena_active()) {
#if !CAPTURE_DIRECT_ENCODE
        /* MJPEG's space holds one YUV buffer at most; the other must already
         * fit, or go where the reorder buffer was (M5Stack). Otherwise giving
         * MJPEG's buffers away only loses them: freed, the heap breaks up and
         * they do not come back (P4-ETH at 1080p). */
        const size_t yuv_bytes = (size_t)H264_MAX_W * H264_MAX_H * 3u / 2u;
        bool second_home = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= yuv_bytes;
#if CAPTURE_YUV_SWAP
        second_home = second_home || capture_yuv_swap_held();
#endif
        if (!second_home) {
            ESP_LOGW(CAPTURE_LOG_TAG, "H.264 does not fit next to MJPEG; MJPEG keeps its buffers");
            return err;
        }
#endif
        capture_mjpeg_release_buffers();
#if CAPTURE_YUV_SWAP
        /* 4 MB next to MJPEG's buffers; alone, their space is too broken up
         * for the 3 MB YUV buffers. */
        capture_yuv_swap_release();
#endif
        ESP_LOGW(CAPTURE_LOG_TAG, "took MJPEG's buffers back and tried again");
        err = h264_open_once();
    }
    return err;
}

static esp_err_t h264_open_once(void)
{
    size_t align = 64;
    (void)esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align);

    const bool arena = capture_arena_active();
    if (arena && !h264_take_arena()) {
        return ESP_ERR_NO_MEM;
    }
    /* The encoded Annex-B output slots are needed on both paths. */
    for (int i = 0; !arena && i < H264_SLOTS; i++) {
        s_buf[i] = esp_h264_aligned_calloc(align, 1, H264_SLOT_CAP, &s_buf_alloc[i],
                                           ESP_H264_MEM_SPIRAM);
        if (!s_buf[i]) {
            ESP_LOGW(CAPTURE_LOG_TAG, "not enough PSRAM for the H.264 output buffers");
            h264_free_buffers();
            return ESP_ERR_NO_MEM;
        }
    }

#if !CAPTURE_DIRECT_ENCODE
    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&ppa_cfg, &s_ppa);
    if (err != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "PPA client: %s", esp_err_to_name(err));
        h264_free_buffers();
        return err;
    }
    const size_t yuv_bytes = (size_t)H264_MAX_W * H264_MAX_H * 3u / 2u;
    for (int i = 0; !arena && i < H264_YUV_BUFS; i++) {
        s_yuv[i] = esp_h264_aligned_calloc(align, 1, yuv_bytes, &s_yuv_alloc[i], ESP_H264_MEM_SPIRAM);
        if (!s_yuv[i]) {
            ESP_LOGW(CAPTURE_LOG_TAG, "not enough PSRAM for the H.264 YUV buffers");
            h264_free_buffers();
            ppa_unregister_client(s_ppa);
            s_ppa = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    /*
     * The allocator zeroed these buffers with the CPU, leaving dirty cache
     * lines. The encoder writes the input buffer back before reading it with
     * DMA, and those stale lines would land on top of what the PPA just put
     * there. Flush them once, here, rather than debug it later.
     */
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        (void)esp_cache_msync(s_yuv[i], s_yuv_alloc[i], ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    /* Two YUV buffers cycle between the PPA stage and the encode task. */
    s_free_slots = xQueueCreate(H264_YUV_BUFS, sizeof(int));
    s_jobs = xQueueCreate(H264_YUV_BUFS, sizeof(h264_job_t));
    s_enc_done = xSemaphoreCreateBinary();
    if (!s_free_slots || !s_jobs || !s_enc_done) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 queues could not be created");
        goto fail;
    }
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        xQueueSend(s_free_slots, &i, 0);
    }
    if (xTaskCreatePinnedToCore(h264_encode_task, "h264enc", 4096, NULL, 5, &s_enc_task, 1) !=
        pdPASS) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 encode task could not start");
        s_enc_task = NULL;
        goto fail;
    }
#endif

    size_t cap = H264_SLOT_CAP;
    for (int i = 0; i < H264_SLOTS; i++) {
        if (s_buf_alloc[i] < cap) {
            cap = s_buf_alloc[i];
        }
    }
    video_frame_install(VIDEO_PAYLOAD_H264, s_buf, H264_SLOTS, cap);
    return ESP_OK;

#if !CAPTURE_DIRECT_ENCODE
fail:
    if (s_jobs) {
        vQueueDelete(s_jobs);
        s_jobs = NULL;
    }
    if (s_free_slots) {
        vQueueDelete(s_free_slots);
        s_free_slots = NULL;
    }
    if (s_enc_done) {
        vSemaphoreDelete(s_enc_done);
        s_enc_done = NULL;
    }
    h264_free_buffers();
    ppa_unregister_client(s_ppa);
    s_ppa = NULL;
    return ESP_ERR_NO_MEM;
#endif
}

static void h264_close(void)
{
#if !CAPTURE_DIRECT_ENCODE
    /* Stop the encode task: a negative-slot job is the sentinel it breaks on.
     * Wait for it to finish whatever it was encoding before freeing anything. */
    if (s_enc_task) {
        const h264_job_t stop = {.slot = -1};
        xQueueSend(s_jobs, &stop, portMAX_DELAY);
        xSemaphoreTake(s_enc_done, portMAX_DELAY);
        s_enc_task = NULL;
    }
    if (s_jobs) {
        vQueueDelete(s_jobs);
        s_jobs = NULL;
    }
    if (s_free_slots) {
        vQueueDelete(s_free_slots);
        s_free_slots = NULL;
    }
    if (s_enc_done) {
        vSemaphoreDelete(s_enc_done);
        s_enc_done = NULL;
    }
#endif
    encoder_park();
    h264_free_buffers();
#if !CAPTURE_DIRECT_ENCODE
    if (s_ppa) {
        (void)ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
#endif
}

/**
 * Make the next frame an IDR.
 *
 * The encoder has no explicit request for one, but it starts a new GOP whenever
 * the configured GOP length differs from the one in force (see
 * h264_hw_enc_gop_mode_process). Alternating between two adjacent lengths is
 * therefore a keyframe request, and costs nothing else.
 */
static void force_idr(void)
{
    if (!s_param) {
        return;
    }
    /* Counted from the request, so the frames before the IDR comes out do not
     * ask again - each GOP change starts another one. */
    s_last_idr_us = esp_timer_get_time();
    /*
     * Toggle between the wanted GOP length and one adjacent to it. Any change of
     * the configured length starts a new GOP, so this forces an IDR while the
     * effective length stays at (or one off) the intended value. Deriving the
     * pair from wanted_gop() every time - rather than stepping s_gop itself - is
     * deliberate: the old code did s_gop-- on each request, so after enough
     * keyframe requests (every reconnecting viewer sends one) the GOP walked down
     * to ~2 and nearly every frame became an IDR, wrecking the bitrate.
     */
    const uint8_t base = wanted_gop();
    const uint8_t alt = (base > 2u) ? (uint8_t)(base - 1u) : (uint8_t)(base + 1u);
    const uint8_t next = (s_gop == base) ? alt : base;
    if (esp_h264_enc_set_gop(&s_param->base, next) == ESP_H264_ERR_OK) {
        s_gop = next;
    }
}

static bool keyframe_overdue(void)
{
    return s_last_idr_us && esp_timer_get_time() - s_last_idr_us > KEYFRAME_MAX_US;
}

static void follow_settings(void)
{
    if (!s_param) {
        return;
    }
    const uint32_t bitrate = wanted_bitrate();
    if (bitrate != s_bitrate) {
        if (esp_h264_enc_set_bitrate(&s_param->base, bitrate) == ESP_H264_ERR_OK) {
            s_bitrate = bitrate;
        }
    }
    /* GOP is left alone: it is also the keyframe-request mechanism, and the
     * encoder applies a changed value at the next IDR anyway. */
}

#if !CAPTURE_DIRECT_ENCODE
/* Encode one filled YUV buffer and publish it. Runs on the encode task, so it
 * overlaps the PPA conversion of the following frame. */
static void h264_encode_job(const h264_job_t *job)
{
    if (s_enc_broken) {
        return; /* the capture loop is already switching to MJPEG */
    }
    if (s_enc_w != job->hres || s_enc_h != job->vres) {
        if (encoder_open(job->hres, job->vres) != ESP_OK) {
            s_enc_broken = true; /* the loop reads this and falls back to MJPEG */
            return;
        }
        /* A fresh encoder starts on an IDR, so nothing else to ask for. */
        (void)video_frame_take_keyframe_request();
    } else if (wedge_rebuild_if_needed(job->hres, job->vres)) {
        return;
    } else if (video_frame_take_keyframe_request() || keyframe_overdue()) {
        force_idr();
    }
    follow_settings();

    const uint32_t padded_w = MB_ALIGN(job->hres);
    const uint32_t padded_h = MB_ALIGN(job->vres);

    int slot = -1;
    uint8_t *dst = NULL;
    size_t cap = 0;
    if (video_frame_begin_write(&slot, &dst, &cap, 1000) != ESP_OK) {
        return;
    }

    const int64_t enc_started_us = esp_timer_get_time();
    esp_h264_enc_in_frame_t in = {
        .raw_data = {.buffer = s_yuv[job->slot],
                     .len = (uint32_t)((size_t)padded_w * padded_h * 3u / 2u)},
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
    };
    esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = dst, .len = (uint32_t)cap}};
    esp_h264_err_t herr = esp_h264_enc_process(s_enc, &in, &out);
    capture_status_add_encode_time((uint32_t)(esp_timer_get_time() - enc_started_us));
    if (herr != ESP_H264_ERR_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "h264 encode: %s", h264_err_name(herr));
        if (herr == ESP_H264_ERR_OVERFLOW || herr == ESP_H264_ERR_MEM) {
            video_frame_request_keyframe();
        }
        return;
    }

    /*
     * Every frame goes out, including the near-empty ones a still screen
     * produces. There is no equivalent of the MJPEG skip here and no need for
     * one: an unchanged screen already costs a few hundred bytes per frame, and
     * a decoder that stops receiving has no way to tell a still picture from a
     * dead link.
     */
    const bool is_idr = out.frame_type == ESP_H264_FRAME_TYPE_IDR;
    wedge_watch(is_idr, out.length);
    if (is_idr) {
        /* Tell decoders frames are never reordered; see h264_sps.h. */
        out.length = (uint32_t)h264_sps_add_restriction(dst, out.length, cap);
    }
    video_frame_publish(slot, out.length, is_idr);
    capture_status_add_frame(out.length);
}

static void h264_encode_task(void *arg)
{
    (void)arg;
    h264_job_t job;
    (void)esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue; /* nothing to encode; the dog still gets fed */
        }
        if (job.slot < 0) {
            break; /* shutdown sentinel from h264_close() */
        }
        if (capture_park_frame_begin()) {
            h264_encode_job(&job);
            capture_park_frame_end();
        }
        /* Hand the YUV buffer back so the PPA stage can fill it again. */
        xQueueSend(s_free_slots, &job.slot, 0);
        /* Give the core away for a tick. When a job is always waiting this task
         * never blocks, the idle task on its core never runs, and the task
         * watchdog restarts the device - seen on a P4-ETH with the console open. */
        vTaskDelay(1);
    }
    (void)esp_task_wdt_delete(NULL);
    xSemaphoreGive(s_enc_done);
    vTaskDelete(NULL);
}
#endif /* !CAPTURE_DIRECT_ENCODE */

/*
 * Capture-loop side: convert the RGB frame into a free YUV buffer with the PPA
 * and hand it to the encode task. Returns as soon as the conversion is done -
 * the encode happens on the other task, overlapping the next conversion. The
 * frame is dropped (its buffer returned) if the encoder has not caught up.
 */
static esp_err_t h264_encode(capture_ctx_t *c, const void *src, bool force_publish)
{
    (void)force_publish;
#if CAPTURE_DIRECT_ENCODE
    /*
     * rev >= 3.0: the encoder consumes the captured pixels directly - no PPA colour
     * conversion, no intermediate YUV buffer. Encode the captured frame in place,
     * synchronously on the capture task: the V1 hold-out keeps that buffer out of the
     * CSI DMA's reach for the whole encode, so nothing overwrites it. The encoder
     * reads the macroblock-aligned frame (height padded to a multiple of 16); the
     * capture buffer is allocated to that padded height and the encoder crops the
     * extra rows in the SPS.
     */
    if (c->hres == 0 || c->vres == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_enc_w != c->hres || s_enc_h != c->vres) {
        const esp_err_t oerr = encoder_open(c->hres, c->vres);
        if (oerr != ESP_OK) {
            s_enc_broken = true;
            /* Pass the reason up: without an encoder there is nothing to retry
             * next frame, and something has to decide to use MJPEG instead. */
            return oerr;
        }
        (void)video_frame_take_keyframe_request();
    } else if (wedge_rebuild_if_needed(c->hres, c->vres)) {
        return ESP_FAIL;
    } else if (video_frame_take_keyframe_request() || keyframe_overdue()) {
        force_idr();
    }
    follow_settings();

    int wslot = -1;
    uint8_t *dst = NULL;
    size_t cap = 0;
    if (video_frame_begin_write(&wslot, &dst, &cap, 1000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    const uint32_t bpad_w = MB_ALIGN(c->hres);
    const uint32_t bpad_h = MB_ALIGN(c->vres);
    const int64_t enc_started_us = esp_timer_get_time();
    esp_h264_enc_in_frame_t in = {
        .raw_data = {.buffer = (uint8_t *)src,
                     .len = (uint32_t)((size_t)bpad_w * bpad_h * capture_pixfmt_bytes())},
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
    };
    esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = dst, .len = (uint32_t)cap}};
    esp_h264_err_t herr = esp_h264_enc_process(s_enc, &in, &out);
    capture_status_add_encode_time((uint32_t)(esp_timer_get_time() - enc_started_us));
    if (herr != ESP_H264_ERR_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "h264 encode: %s", h264_err_name(herr));
        if (herr == ESP_H264_ERR_OVERFLOW || herr == ESP_H264_ERR_MEM) {
            video_frame_request_keyframe();
        }
        return ESP_FAIL;
    }
    const bool is_idr = out.frame_type == ESP_H264_FRAME_TYPE_IDR;
    wedge_watch(is_idr, out.length);
    if (is_idr) {
        out.length = (uint32_t)h264_sps_add_restriction(dst, out.length, cap);
    }
    video_frame_publish(wslot, out.length, is_idr);
    capture_status_add_frame(out.length);
    return ESP_OK;
#else
    /* The encoder is built on the encode task, so its failure only reaches the
     * capture loop through here. Without this the loop never fell back and
     * the encode task retried the build on every frame. */
    if (s_enc_broken) {
        return ESP_ERR_NO_MEM;
    }
#if CAPTURE_YUV_SWAP
    if (!s_yuv[0] || !s_free_slots || !s_jobs) {
#else
    if (!s_ppa || !s_yuv[0] || !s_free_slots || !s_jobs) {
#endif
        return ESP_ERR_INVALID_STATE;
    }
    if (c->hres == 0 || c->vres == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int slot = -1;
    if (xQueueReceive(s_free_slots, &slot, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* The encoder is still busy with both buffers; skip this frame rather
         * than stall the capture loop. */
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t padded_w = MB_ALIGN(c->hres);
    const uint32_t padded_h = MB_ALIGN(c->vres);
    const int64_t ppa_started_us = esp_timer_get_time();

    /*
     * scale_x / scale_y stay at 1.0. A PPA transaction that scales while
     * writing YUV420 never completes - the blocking call waits forever and
     * takes the capture task with it. Downscaling, if it is ever wanted, has to
     * be a separate RGB pass.
     *
     * The destination picture is the macroblock-aligned size while the written
     * block is the real one, which puts each row at the stride the encoder
     * expects and leaves the padding rows untouched.
     */
#if CAPTURE_YUV_SWAP
    (void)padded_h;
    capture_yuv422_to_h264((const uint8_t *)src, s_yuv[slot], c->hres, c->vres, padded_w);
    (void)esp_cache_msync(s_yuv[slot], s_yuv_alloc[slot], ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    capture_status_add_ppa_time((uint32_t)(esp_timer_get_time() - ppa_started_us));
    /*
     * Hand the core back for a tick. Every other path here waits on hardware -
     * the PPA and both encoders block on a DMA - and that wait is what lets the
     * idle task run. This one is the only pure CPU pass in the pipeline, and at
     * 37 ms a frame against 33 ms between them the capture task would never
     * block again: the idle task stops being scheduled, and five seconds later
     * the task watchdog reboots the device. Measured, twice.
     */
    vTaskDelay(1);
#else
    ppa_srm_oper_config_t srm = {
        .in = {.buffer = (void *)src,
               .pic_w = c->hres,
               .pic_h = c->vres,
               .block_w = c->hres,
               .block_h = c->vres,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
        .out = {.buffer = s_yuv[slot],
                .buffer_size = s_yuv_alloc[slot],
                .pic_w = padded_w,
                .pic_h = padded_h,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &srm);
    capture_status_add_ppa_time((uint32_t)(esp_timer_get_time() - ppa_started_us));
    if (err != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "ppa rgb->yuv: %s", esp_err_to_name(err));
        xQueueSend(s_free_slots, &slot, 0); /* return the unused buffer */
        return err;
    }
#endif

    const h264_job_t job = {.slot = slot, .hres = c->hres, .vres = c->vres};
    if (xQueueSend(s_jobs, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xQueueSend(s_free_slots, &slot, 0);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
#endif
}

static const capture_codec_t s_h264 = {
    .name = "h264",
    .payload = VIDEO_PAYLOAD_H264,
    .open = h264_open,
    .close = h264_close,
    .encode = h264_encode,
};

const capture_codec_t *capture_codec_h264(void)
{
    return &s_h264;
}
