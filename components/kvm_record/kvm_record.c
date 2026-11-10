/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two tasks and a ring of frames between them.
 *
 * The feed takes each H.264 frame as it is published and adds it to a ring in
 * PSRAM. The writer takes frames off the ring, muxes them into transport stream
 * packets and writes them to the card. The ring is what lets a card that pauses
 * for a second cost nothing: the feed never waits for the card, so the stream
 * never waits for it either.
 *
 * With the dashcam on, the feed runs all the time and the ring is the last
 * minutes of the screen, oldest group of pictures dropped first. A clip is then
 * a recording that starts with what the ring already holds.
 *
 * The dashcam can keep its past on the card instead ("microSD"): the writer runs
 * in the background, cutting the stream into short segments in VIDEO/.dashcam
 * and deleting the ones too old to matter. A clip is then a list of segments,
 * which the converter joins into one MP4. That needs no big ring, so it works on
 * boards with little PSRAM, and reaches back as far as the setting says.
 *
 * A frame that cannot be kept (the ring is full while recording, or the stream
 * skipped one) breaks the chain of H.264 references, so everything after it is
 * left out until the next keyframe, and one is asked for at once. The file then
 * jumps rather than showing a smeared picture.
 */
#include "kvm_record.h"
#include "kvm_upload.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "frame_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "keylog.h"
#include "kvm_audio.h"
#include "kvm_settings.h"
#include "kvm_storage.h"
#include "record_priv.h"
#include "screentext.h"
#include "screentext_store.h"
#include "ts_mux.h"
#include "usb_hid.h"
#include "video_frame.h"

#define TAG "record"

/* The dashcam's ring: about three seconds of the default 12 Mbit/s stream,
 * minutes of a still one. */
#define RING_BYTES_MAX (5 * 1024 * 1024)
#define RING_BYTES_MIN (1536 * 1024)
/* PSRAM the dashcam leaves for everyone else: a screenshot takes 2.6 MB for a moment. */
#define PSRAM_RESERVE (4 * 1024 * 1024)
/* A recording only needs the ring to cover the card's pauses, so it makes do
 * with less and leaves less: a pre-3.0 board has about 3 MB of PSRAM to spare
 * while H.264 runs. What it leaves is enough for a screenshot. */
#define REC_RING_BYTES_MAX (2 * 1024 * 1024)
#define REC_RING_BYTES_MIN (384 * 1024)
#define REC_PSRAM_RESERVE (1536 * 1024)
#define RING_FRAMES 4096
/* The biggest H.264 frame the encoder can hand out: its slot size. */
#define FRAME_MAX (1920u * 1080u / 2u)
/* Written in blocks this size, cache-aligned, so each is one DMA multi-block write. */
#define WRITE_CHUNK (256 * 1024)
/* Files stay under 4 GiB, on exFAT too: sizes are 32-bit in the web API and
 * the MP4 writer. A new file starts before that, on a keyframe. */
#define PART_MAX_BYTES (3900ull * 1024 * 1024)
/* How often the file's size is committed to the directory. FAT records it only
 * on a sync, so a power cut loses what came after the last one. */
#define SYNC_US (5 * 1000 * 1000)
/* Refuse to start on a card with less room than this. */
#define MIN_FREE_BYTES (32ull * 1024 * 1024)
/* A dashcam clip that keeps being extended by new events stops here. */
#define EVENT_MAX_US (10 * 60 * 1000000LL)
#define FEED_PRIO (tskIDLE_PRIORITY + 6)
#define WRITER_PRIO (tskIDLE_PRIORITY + 4)
/* A timelapse plays at this rate, whatever its interval. */
#define TIMELAPSE_FPS 25u

static SemaphoreHandle_t s_ctl;         /* start, stop, the feed's life */
static SemaphoreHandle_t s_writer_done;
static volatile bool s_active;          /* a recording is being written */
static volatile bool s_stop;
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;
static kvm_record_status_t s_st;
static int64_t s_started_us;

/* The ring and the feed. s_ring_mu guards the ring's contents. */
static SemaphoreHandle_t s_ring_mu;
static SemaphoreHandle_t s_ring_ready;  /* given when a frame is added */
static frame_ring_t s_ring;
static uint8_t *s_ring_buf;
static bool s_ring_lent; /* s_ring_buf and s_frame are the codec region's tail */
static frame_ring_entry_t *s_ring_entries;
static volatile bool s_feed_running;
static volatile bool s_dashcam;
static volatile bool s_dashcam_no_memory; /* switched on, but the ring did not fit */
static volatile bool s_feed_resync;     /* drop until a keyframe: the ring was cleared */
/* Set when a codec could not get memory: the ring stays gone for a while. */
static volatile int64_t s_ring_hold_until_us;

/* Writer state - only the writer task touches these once it runs. */
static FILE *s_file;
static char s_base[48]; /* the name without extension, e.g. "20260917-140322" */
static int s_part;
static uint8_t *s_chunk;
static uint8_t *s_frame;
static size_t s_frame_cap; /* a frame bigger than the ring never enters it */
static size_t s_chunk_len;
static uint64_t s_file_bytes;
static bool s_write_failed;
static int64_t s_limit_us; /* stop this long after the first frame, 0 = until stopped */
static int64_t s_split_us; /* a new file after this long, 0 = only at the FAT32 limit */
/* A timelapse keeps one keyframe every s_every_us and plays them at TIMELAPSE_FPS. */
static int64_t s_every_us;  /* 0 = an ordinary recording */
static int64_t s_due_us;    /* the next keyframe at or after this goes in */
static uint32_t s_tl_frame; /* frames written into this part */
static char s_limit_why[64];
/* The target's sound, when audio is on: the Opus packets up to a picture's time
 * go in just before it. A timelapse has none. The dashcam in memory keeps the
 * encoder running, and its past in the encoder's buffer, between clips.
 * s_snd_mu guards opening and closing it. */
#define DASHCAM_AUDIO_S 120
static SemaphoreHandle_t s_snd_mu;
static kvm_opus_rec_t *s_snd;
static kvm_opus_packet_t *s_snd_pkt; /* read, not written yet when s_snd_held */
static bool s_snd_held;
static bool s_snd_dashcam;  /* opened for the dashcam, with its minutes of past */
static bool s_writer_audio; /* the writer runs and closes the encoder when done */
static bool s_rec_audio;    /* this recording has sound */

/* The dashcam on the card: the writer is running for it, not for the operator. */
#define SEG_DIR KVM_RECORD_DIR "/.dashcam"
#define SEG_US (15 * 1000000LL)
#define SEGS_MAX 64
#define SEG_PINS 4
typedef struct {
    uint32_t n;
    int64_t t0_us; /* first frame, 0 until it arrives */
    int64_t t1_us; /* when the next segment began, 0 while this one is written */
} seg_t;
static volatile bool s_background;
static seg_t s_segs[SEGS_MAX]; /* oldest first */
static int s_seg_count;
static uint32_t s_seg_next = 1;
static uint32_t s_seg_pins[SEG_PINS]; /* first segment of a clip still being made; 0 = free */
static uint32_t s_event_first_seg;
static char s_event_base[48];
static char s_bg_stopped[96];

/* A dashcam clip: it ends at a time rather than after a length. */
static volatile bool s_event;
static volatile int64_t s_event_end_us;
static int64_t s_event_started_us;

static record_chapter_t s_chapters[RECORD_CHAPTERS_MAX];
static int s_chapter_count;
static void (*s_finished_cb)(const record_finished_t *done);

/*
 * Subtitles. Every report the device sends to the target is copied into a queue
 * on the sender's task; the writer turns them into cues and writes them into a
 * .srt beside the video, timed from the part's first frame.
 */
typedef struct {
    usb_hid_obs_t report;
    int64_t at_us;
} key_event_t;
#define KEY_EVENTS 128
static QueueHandle_t s_key_events;
static volatile bool s_subs_on;
/* Cues are being built for the dashcam's past, with no file to write them to. */
static volatile bool s_cues_on;
static SemaphoreHandle_t s_keylog_mu;
static FILE *s_srt;
static int s_srt_index;
static char s_srt_path[112];

/*
 * The screen's text, when it has any, written beside the video: "<ms>\t<the
 * screen on one line>" whenever it changes. That is what the panel searches to
 * find the moment a message was on screen.
 */
#define TEXT_EVERY_US (3 * 1000000LL)
#define TEXT_MAX 12288
static FILE *s_txt;
static screentext_grid_t *s_text_grid;
static char *s_text_buf;
static uint32_t s_text_hash;
static int64_t s_text_next_us;
static int s_text_lines;
static char s_text_path[112];
static int64_t s_part_t0_us; /* first frame of the part, 0 until it arrives */
static keylog_t s_keylog;

static void set_stopped(const char *why)
{
    portENTER_CRITICAL(&s_mu);
    /* The background writer's reasons are its own; the operator's last
     * recording keeps what it said. */
    char *to = s_background ? s_bg_stopped : s_st.stopped;
    if (!to[0] && why) {
        snprintf(to, sizeof(s_st.stopped), "%s", why);
    }
    portEXIT_CRITICAL(&s_mu);
}

/* ---- names ------------------------------------------------------------------ */

/* A time stamp when the clock has been set, the uptime when it has not. */
static void stamp(char *out, size_t cap)
{
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) {
        strftime(out, cap, "%Y%m%d-%H%M%S", &tm);
    } else {
        snprintf(out, cap, "up-%06lu", (unsigned long)(esp_timer_get_time() / 1000000));
    }
}

static bool ensure_dir(const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), dir);
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path, 0775) == 0;
}

/* dir/stamp.ext, or dir/stamp-2.ext if two land in the same second. */
static bool unique_name(const char *dir, const char *ext, char *base, size_t base_cap, char *rel,
                        size_t rel_cap)
{
    char st[24];
    stamp(st, sizeof(st));
    for (int i = 1; i < 100; i++) {
        if (i == 1) {
            snprintf(base, base_cap, "%s", st);
        } else {
            snprintf(base, base_cap, "%s-%d", st, i);
        }
        snprintf(rel, rel_cap, "%s/%s.%s", dir, base, ext);
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), rel);
        struct stat s;
        if (stat(path, &s) != 0) {
            return true;
        }
    }
    return false;
}

static uint64_t card_free_bytes(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(kvm_storage_mount_point(), &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return free_bytes;
}

/* ---- subtitles --------------------------------------------------------------- */

static void on_hid_report(const usb_hid_obs_t *report)
{
    if ((!s_subs_on && !s_cues_on) || !s_key_events) {
        return;
    }
    const key_event_t ev = {.report = *report, .at_us = esp_timer_get_time()};
    (void)xQueueSend(s_key_events, &ev, 0); /* a full queue loses a report, never the stream */
}

static void srt_time(char *out, size_t cap, int64_t us)
{
    if (us < 0) {
        us = 0;
    }
    const int64_t ms = us / 1000;
    snprintf(out, cap, "%02lld:%02lld:%02lld,%03lld", (long long)(ms / 3600000),
             (long long)(ms / 60000 % 60), (long long)(ms / 1000 % 60), (long long)(ms % 1000));
}

/*
 * The dashcam on the card has no file to write cues into while it runs - a clip
 * is decided later - so it keeps the recent ones here, by time, and a clip
 * writes the ones it spans.
 */
#define BG_CUES 256
#define BG_CUE_TEXT 80
typedef struct {
    int64_t start_us, end_us;
    char text[BG_CUE_TEXT];
} bg_cue_t;
static bg_cue_t *s_bg_cues; /* a ring in PSRAM, oldest at s_bg_cue_head */
static int s_bg_cue_head;
static int s_bg_cue_count;

/* Keep a cue for later: the dashcam's past has no file open yet. */
static void cue_remember(int64_t start_us, int64_t end_us, const char *text)
{
    if (s_bg_cues) {
        const int at = (s_bg_cue_head + s_bg_cue_count) % BG_CUES;
        s_bg_cues[at] = (bg_cue_t){.start_us = start_us, .end_us = end_us};
        snprintf(s_bg_cues[at].text, BG_CUE_TEXT, "%s", text);
        if (s_bg_cue_count < BG_CUES) {
            s_bg_cue_count++;
        } else {
            s_bg_cue_head = (s_bg_cue_head + 1) % BG_CUES;
        }
    }
}

static void on_cue(void *user, int64_t start_us, int64_t end_us, const char *text)
{
    (void)user;
    if (!s_srt) {
        /* No file: this is the dashcam's past, kept in case a clip wants it. */
        cue_remember(start_us, end_us, text);
        return;
    }
    /* A key pressed before the first frame arrived shows at the very start. */
    const int64_t t0 = s_part_t0_us ? s_part_t0_us : start_us;
    char a[24], b[24];
    srt_time(a, sizeof(a), start_us - t0);
    srt_time(b, sizeof(b), end_us - t0);
    fprintf(s_srt, "%d\n%s --> %s\n%s\n\n", ++s_srt_index, a, b, text);
}

/* A clip from the card: its cues, timed from its first frame at @p t0_us. */
static void write_clip_srt(const char *ts_rel, int64_t t0_us, int64_t end_us)
{
    if (!s_bg_cues || !s_bg_cue_count || !t0_us) {
        return;
    }
    keylog_flush(&s_keylog);
    char path[112];
    snprintf(path, sizeof(path), "%s/%.*s.srt", kvm_storage_mount_point(), (int)(strlen(ts_rel) - 3), ts_rel);
    FILE *f = NULL;
    int n = 0;
    for (int i = 0; i < s_bg_cue_count; i++) {
        const bg_cue_t *c = &s_bg_cues[(s_bg_cue_head + i) % BG_CUES];
        if (c->end_us <= t0_us || c->start_us >= end_us) {
            continue;
        }
        if (!f && !(f = fopen(path, "w"))) {
            return;
        }
        char a[24], b[24];
        srt_time(a, sizeof(a), c->start_us - t0_us);
        srt_time(b, sizeof(b), c->end_us - t0_us);
        fprintf(f, "%d\n%s --> %s\n%s\n\n", ++n, a, b, c->text);
    }
    if (f) {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

static void open_srt(const char *ts_rel)
{
    char path[112];
    snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), ts_rel);
    const size_t n = strlen(path);
    if (n > 3 && strcmp(path + n - 3, ".ts") == 0) {
        strcpy(path + n - 3, ".srt");
    }
    s_srt = fopen(path, "w");
    s_srt_index = 0;
    snprintf(s_srt_path, sizeof(s_srt_path), "%s", path);
    if (!s_srt) {
        ESP_LOGW(TAG, "no subtitles: could not create %s", path);
    }
}

/* ---- the screen's text ---------------------------------------------------- */

static void open_txt(const char *ts_rel)
{
    if (!s_text_grid) {
        return;
    }
    snprintf(s_text_path, sizeof(s_text_path), "%s/%.*s.txt", kvm_storage_mount_point(),
             (int)(strlen(ts_rel) - 3), ts_rel);
    s_txt = fopen(s_text_path, "w");
    s_text_hash = 0;
    s_text_next_us = 0;
    s_text_lines = 0;
}

static void close_txt(void)
{
    if (s_txt) {
        fflush(s_txt);
        fsync(fileno(s_txt));
        fclose(s_txt);
        s_txt = NULL;
        /* A picture all the way through: nothing to search, no file to keep. */
        if (!s_text_lines) {
            remove(s_text_path);
        }
    }
}

/*
 * A reading, now and then, written when it differs from the last one. The read
 * itself happens on the capture task; this only asks for it and takes the
 * result, so a screen that is a picture costs a sample of pixels there.
 */
static void text_tick(int64_t now_us)
{
    if (!s_txt || !s_text_grid || !s_text_buf || now_us < s_text_next_us) {
        return;
    }
    s_text_next_us = now_us + TEXT_EVERY_US;
    screentext_request();
    uint32_t age_ms = 0;
    if (!screentext_latest(s_text_grid, &age_ms) || age_ms > 4000) {
        return;
    }
    const size_t n = screentext_to_utf8(s_text_grid, s_text_buf, TEXT_MAX);
    if (!n) {
        return;
    }
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        if (s_text_buf[i] == '\n' || s_text_buf[i] == '\t') {
            s_text_buf[i] = ' '; /* one screen, one line */
        }
        hash = (hash ^ (uint8_t)s_text_buf[i]) * 16777619u;
    }
    if (hash == s_text_hash) {
        return;
    }
    s_text_hash = hash;
    const int64_t at_ms = s_part_t0_us ? (now_us - s_part_t0_us) / 1000 : 0;
    fprintf(s_txt, "%lld\t%s\n", (long long)at_ms, s_text_buf);
    s_text_lines++;
}

static void close_srt(void)
{
    if (s_srt) {
        fflush(s_srt);
        fsync(fileno(s_srt));
        fclose(s_srt);
        s_srt = NULL;
        /* Nothing was pressed: no empty file beside the video. */
        if (s_srt_index == 0) {
            remove(s_srt_path);
        }
    }
}

/*
 * Feed what arrived to the cue builder. The writer does this while it records;
 * the feed task does it the rest of the time, so the dashcam's past has
 * keystrokes in it. The lock is for the moment a recording starts or stops,
 * when both tasks could reach for the same cue builder.
 */
static void drain_key_events(int64_t now_us)
{
    if (!s_subs_on && !s_cues_on) {
        return;
    }
    if (s_keylog_mu && xSemaphoreTake(s_keylog_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    key_event_t ev;
    while (xQueueReceive(s_key_events, &ev, 0) == pdTRUE) {
        const usb_hid_obs_t *r = &ev.report;
        switch (r->type) {
        case USB_HID_OBS_KEYBOARD:
            keylog_keyboard(&s_keylog, ev.at_us, r->modifier, r->keys);
            break;
        case USB_HID_OBS_MOUSE_ABS: {
            kvm_video_status_t vs;
            capture_status_get(&vs);
            const int x = vs.hres ? (int)((uint32_t)r->x * vs.hres / 32768u) : -1;
            const int y = vs.vres ? (int)((uint32_t)r->y * vs.vres / 32768u) : -1;
            keylog_mouse(&s_keylog, ev.at_us, r->buttons, x, y);
            break;
        }
        case USB_HID_OBS_MOUSE_REL:
            keylog_mouse(&s_keylog, ev.at_us, r->buttons, -1, -1);
            break;
        case USB_HID_OBS_CONSUMER:
            keylog_consumer(&s_keylog, ev.at_us, r->usage);
            break;
        }
    }
    keylog_tick(&s_keylog, now_us);
    if (s_keylog_mu) {
        xSemaphoreGive(s_keylog_mu);
    }
}

/* The cue builder, started from the settings. Under s_ctl or at start-up. */
static bool keylog_begin(void)
{
    const keylog_mode_t subs = (keylog_mode_t)kvm_setting_int("rec_subs");
    if (subs == KEYLOG_OFF || !s_key_events) {
        return false;
    }
    const kvm_setting_t *layout = kvm_setting_find("kbd_layout");
    const int li = kvm_setting_int("kbd_layout");
    const char *layout_id =
        (layout && layout->choices && li >= 0 && li <= layout->max) ? layout->choices[li] : "en_us";
    keylog_init(&s_keylog, subs, kvm_setting_bool("rec_clicks"), layout_id, on_cue, NULL);
    xQueueReset(s_key_events);
    return true;
}

/* Cues kept for the past, written into an open .srt from @p from_us. */
static void replay_cues(int64_t from_us)
{
    if (!s_srt || !s_bg_cues) {
        return;
    }
    for (int i = 0; i < s_bg_cue_count; i++) {
        const bg_cue_t *c = &s_bg_cues[(s_bg_cue_head + i) % BG_CUES];
        if (c->end_us > from_us) {
            on_cue(NULL, c->start_us, c->end_us, c->text);
        }
    }
}

/* ---- the ring ------------------------------------------------------------------ */

/* Memory for the ring, sized to what PSRAM can spare: the dashcam's big one,
 * or a recording's small one. Under s_ctl. */
static bool ring_alloc(bool dashcam)
{
    if (s_ring_buf) {
        return true;
    }
    const size_t reserve = dashcam ? PSRAM_RESERVE : REC_PSRAM_RESERVE;
    const size_t min = dashcam ? RING_BYTES_MIN : REC_RING_BYTES_MIN;
    const size_t max = dashcam ? RING_BYTES_MAX : REC_RING_BYTES_MAX;

    /* First the part of the codec region H.264 leaves unused: taken and given
     * back whole, it breaks nothing up. */
    size_t lent = 0;
    uint8_t *tail = capture_arena_borrow(min + FRAME_MAX, &lent);
    if (tail) {
        s_ring_entries = heap_caps_malloc(RING_FRAMES * sizeof(frame_ring_entry_t), MALLOC_CAP_SPIRAM);
        if (!s_ring_entries) {
            capture_arena_give_back(tail);
            return false;
        }
        size_t bytes = lent - FRAME_MAX;
        if (bytes > max) {
            bytes = max;
        }
        s_ring_lent = true;
        s_ring_buf = tail;
        s_frame = tail + bytes; /* both 64-aligned: the region's pieces are */
        s_frame_cap = FRAME_MAX;
        frame_ring_init(&s_ring, s_ring_buf, bytes, s_ring_entries, RING_FRAMES);
        ESP_LOGI(TAG, "frame ring: %u KB, in the codec region", (unsigned)(bytes / 1024));
        return true;
    }
    /* The write chunk is already taken by the time a recording gets here. */
    const size_t entries = RING_FRAMES * sizeof(frame_ring_entry_t);
    const size_t overhead = reserve + entries;
    size_t free_psram = 0;
    size_t bytes = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt) {
            /* Short of a full ring: MJPEG keeps its buffers while H.264
             * runs, so ask for them. */
            capture_release_idle_buffers();
        }
        free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        /* The ring and a scratch copy of its biggest frame, which is never
         * bigger than the ring: so a small ring costs twice its size, a big one
         * its size plus FRAME_MAX. */
        const size_t avail = free_psram > overhead ? free_psram - overhead : 0;
        bytes = avail >= 2 * FRAME_MAX ? avail - FRAME_MAX : avail / 2;
        if (bytes > max) {
            bytes = max;
        }
        /* Free is not contiguous: after a few codec switches 5 MB is free in
         * pieces and the malloc below would fail. */
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (bytes > largest) {
            bytes = largest;
        }
        if (bytes >= (attempt ? min : max)) {
            break;
        }
    }
    if (bytes < min) {
        ESP_LOGW(TAG, "%u KB of PSRAM free; a %s frame ring needs %u KB", (unsigned)(free_psram / 1024),
                 dashcam ? "full dashcam" : "small", (unsigned)((overhead + 2 * min) / 1024));
        return false;
    }
    /* On boards where a screenshot needs one 4 MB block, the ring stays small
     * and must not take that block: the operator chose screenshots over a
     * longer look-back (2026-09-23). */
    const size_t keep = capture_psram_keep_block();
    if (keep && bytes > REC_RING_BYTES_MAX) {
        bytes = REC_RING_BYTES_MAX;
    }
    for (;;) {
        s_frame_cap = bytes < FRAME_MAX ? bytes : FRAME_MAX;
        s_ring_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        s_ring_entries = heap_caps_malloc(RING_FRAMES * sizeof(frame_ring_entry_t), MALLOC_CAP_SPIRAM);
        s_frame = heap_caps_malloc(s_frame_cap, MALLOC_CAP_SPIRAM);
        const bool got = s_ring_buf && s_ring_entries && s_frame;
        if (got && (!keep || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= keep)) {
            break;
        }
        heap_caps_free(s_ring_buf);
        heap_caps_free(s_ring_entries);
        heap_caps_free(s_frame);
        s_ring_buf = NULL;
        s_ring_entries = NULL;
        s_frame = NULL;
        if (!got || bytes / 2 < min) {
            return false;
        }
        bytes /= 2;
    }
    frame_ring_init(&s_ring, s_ring_buf, bytes, s_ring_entries, RING_FRAMES);
    ESP_LOGI(TAG, "frame ring: %u KB", (unsigned)(bytes / 1024));
    return true;
}

static void ring_free(void)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    if (s_ring_lent) {
        capture_arena_give_back(s_ring_buf);
        s_ring_lent = false;
    } else {
        heap_caps_free(s_ring_buf);
        heap_caps_free(s_frame);
    }
    heap_caps_free(s_ring_entries);
    s_ring_buf = NULL;
    s_ring_entries = NULL;
    s_frame = NULL;
    memset(&s_ring, 0, sizeof(s_ring));
    xSemaphoreGive(s_ring_mu);
}

static void dropped_one(void)
{
    portENTER_CRITICAL(&s_mu);
    s_st.dropped++;
    portEXIT_CRITICAL(&s_mu);
}

static void feed_task(void *arg)
{
    (void)arg;
    video_frame_viewer_enter();
    video_frame_request_keyframe();
    uint32_t last_seq = video_frame_seq();
    bool need_key = true;
    int64_t asked_us = esp_timer_get_time();

    for (;;) {
        if (!s_active && !s_dashcam) {
            /* Leaving is decided under s_ctl, so a start cannot slip in between. */
            xSemaphoreTake(s_ctl, portMAX_DELAY);
            if (!s_active && !s_dashcam) {
                s_feed_running = false;
                xSemaphoreGive(s_ctl);
                break;
            }
            xSemaphoreGive(s_ctl);
        }
        /* Nobody is writing a file, so the cues are this task's to build. */
        if (!s_active && s_cues_on) {
            drain_key_events(esp_timer_get_time());
        }
        if (s_feed_resync) {
            s_feed_resync = false;
            need_key = true;
            video_frame_request_keyframe();
        }
        if (!video_frame_wait_new(last_seq, 500)) {
            /* No frames: maybe no codec runs at all - the other one could not get
             * memory. Do not be what keeps it from getting it. */
            if (!s_active && s_ring_buf && video_frame_payload() != VIDEO_PAYLOAD_H264) {
                ring_free();
                need_key = true;
            }
            continue;
        }
        if (!s_ring_buf) {
            /* The ring is taken only once H.264 is running, never while another
             * codec may still be asking for its buffers - and only while someone
             * still wants it: the dashcam may have been switched off a moment ago
             * precisely to free it. */
            if (video_frame_payload() != VIDEO_PAYLOAD_H264 || (!s_dashcam && !s_active)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                last_seq = video_frame_seq();
                continue;
            }
            if (esp_timer_get_time() < s_ring_hold_until_us) {
                /* A codec just failed for want of memory; leave it room. */
                vTaskDelay(pdMS_TO_TICKS(500));
                last_seq = video_frame_seq();
                continue;
            }
            xSemaphoreTake(s_ctl, portMAX_DELAY);
            /* A board short of PSRAM gets the recording's small ring: seconds of
             * past instead of a minute, rather than no dashcam at all. */
            const bool got = (s_dashcam || s_active) &&
                             (ring_alloc(s_dashcam) || (s_dashcam && ring_alloc(false)));
            if (!got && s_dashcam) {
                s_dashcam = false;
                s_dashcam_no_memory = true;
                ESP_LOGW(TAG, "dashcam: not enough PSRAM for the frame ring");
            }
            xSemaphoreGive(s_ctl);
            need_key = true;
            last_seq = video_frame_seq();
            continue;
        }
        video_frame_ref_t f;
        if (!video_frame_acquire(&f)) {
            vTaskDelay(1);
            last_seq = video_frame_seq();
            continue;
        }
        if (f.payload != VIDEO_PAYLOAD_H264) {
            video_frame_release(&f);
            if (s_active) {
                set_stopped("the video codec was switched away from H.264");
                s_stop = true;
                xSemaphoreGive(s_ring_ready);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue; /* let the writer close before the ring goes */
            }
            /*
             * Not H.264: nothing to keep. Stop being a viewer, so the other codec
             * does not encode for nobody, and give the ring's PSRAM back - MJPEG
             * needs megabytes of it. Both come back when H.264 does.
             */
            video_frame_viewer_leave();
            ring_free();
            while ((s_dashcam || s_active) && video_frame_payload() != VIDEO_PAYLOAD_H264) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            /* The ring comes back at the top of the loop, once H.264 is running. */
            video_frame_viewer_enter();
            video_frame_request_keyframe();
            need_key = true;
            last_seq = video_frame_seq();
            continue;
        }
        const bool gap = (uint32_t)(f.seq - last_seq) > 1u;
        last_seq = f.seq;
        if (gap && !need_key) {
            need_key = true;
            if (s_active) {
                dropped_one();
            }
        }
        if (need_key && !f.keyframe) {
            video_frame_release(&f);
            /* Asked again now and then, in case a request was lost to a codec restart. */
            const int64_t now = esp_timer_get_time();
            if (now - asked_us > 1000000) {
                asked_us = now;
                video_frame_request_keyframe();
            }
            continue;
        }
        /* Recording: a full ring means the card is behind - drop, never the past.
         * Waiting for an event: drop the oldest past to make room. */
        const bool evict = !s_active;
        xSemaphoreTake(s_ring_mu, portMAX_DELAY);
        const bool kept = s_ring_buf &&
                          frame_ring_push(&s_ring, f.data, f.len, f.at_us, f.keyframe, evict);
        xSemaphoreGive(s_ring_mu);
        video_frame_release(&f);
        if (!kept) {
            if (s_active) {
                dropped_one();
            }
            need_key = true;
            asked_us = esp_timer_get_time();
            video_frame_request_keyframe();
            continue;
        }
        xSemaphoreGive(s_ring_ready);
        need_key = false;
    }
    video_frame_viewer_leave();
    ring_free();
    vTaskDelete(NULL);
}

/* Start the feed if it is not running. Under s_ctl. */
static bool feed_ensure(void)
{
    /* A dashcam that cannot get its ring must not also stop a plain recording. */
    if (!ring_alloc(s_dashcam) && !(s_dashcam && ring_alloc(false))) {
        return false;
    }
    if (s_feed_running) {
        return true;
    }
    s_feed_running = true;
    if (xTaskCreate(feed_task, "kvm_rec_feed", 4096, NULL, FEED_PRIO, NULL) != pdPASS) {
        s_feed_running = false;
        return false;
    }
    return true;
}

/* ---- writer ------------------------------------------------------------------ */

static void write_chunk(void)
{
    if (!s_chunk_len) {
        return;
    }
    if (!s_write_failed && s_file) {
        if (fwrite(s_chunk, 1, s_chunk_len, s_file) != s_chunk_len) {
            const int e = errno;
            s_write_failed = true;
            if (e == ENOSPC || card_free_bytes() < 1024u * 1024u) {
                set_stopped("the card is full");
            } else {
                char why[96];
                snprintf(why, sizeof(why), "writing to the card failed (%s)", strerror(e));
                set_stopped(why);
            }
            ESP_LOGW(TAG, "write failed: %s", strerror(e));
            s_stop = true;
        } else {
            s_file_bytes += s_chunk_len;
            portENTER_CRITICAL(&s_mu);
            s_st.bytes += s_chunk_len;
            portEXIT_CRITICAL(&s_mu);
        }
    }
    s_chunk_len = 0;
}

static void on_packet(void *user, const uint8_t *packet)
{
    (void)user;
    if (s_chunk_len + TS_PACKET > WRITE_CHUNK) {
        write_chunk();
    }
    memcpy(s_chunk + s_chunk_len, packet, TS_PACKET);
    s_chunk_len += TS_PACKET;
}

static void stop_writer(const char *reason);
static esp_err_t seg_mark_event(const char *title, uint32_t pre_s, uint32_t post_s, char *why,
                                size_t why_cap);

/* ---- segments ------------------------------------------------------------------ */

static void seg_path(uint32_t n, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%06u.ts", kvm_storage_mount_point(), SEG_DIR, (unsigned)(n % 1000000u));
}

static uint32_t seg_pinned_from(void)
{
    uint32_t from = UINT32_MAX;
    for (int i = 0; i < SEG_PINS; i++) {
        if (s_seg_pins[i] && s_seg_pins[i] < from) {
            from = s_seg_pins[i];
        }
    }
    return from;
}

static void seg_drop_oldest(void)
{
    char path[112];
    seg_path(s_segs[0].n, path, sizeof(path));
    remove(path);
    portENTER_CRITICAL(&s_mu);
    memmove(&s_segs[0], &s_segs[1], (size_t)(s_seg_count - 1) * sizeof(seg_t));
    s_seg_count--;
    portEXIT_CRITICAL(&s_mu);
}

/*
 * Delete what is too old to go into a clip: a segment goes once the one after it
 * already starts before the window. Not a segment a clip still needs, unless the
 * table is full or the card is.
 */
static void seg_prune(int64_t now_us)
{
    const int64_t window_from = now_us - (int64_t)kvm_setting_int("dashcam_pre_s") * 1000000;
    const uint32_t pinned = seg_pinned_from();
    while (s_seg_count > 1 && s_segs[0].n < pinned && s_segs[1].t0_us && s_segs[1].t0_us <= window_from) {
        seg_drop_oldest();
    }
    while (s_seg_count >= SEGS_MAX - 1) {
        seg_drop_oldest();
    }
    while (s_seg_count > 1 && s_segs[0].n < pinned && card_free_bytes() < MIN_FREE_BYTES) {
        seg_drop_oldest();
    }
}

static FILE *seg_open(char *rel, size_t rel_cap)
{
    const uint32_t n = s_seg_next++;
    snprintf(rel, rel_cap, "%s/%06u.ts", SEG_DIR, (unsigned)(n % 1000000u));
    char path[112];
    seg_path(n, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        return NULL;
    }
    setvbuf(f, NULL, _IONBF, 0);
    portENTER_CRITICAL(&s_mu);
    if (s_seg_count < SEGS_MAX) {
        s_segs[s_seg_count++] = (seg_t){.n = n};
    }
    portEXIT_CRITICAL(&s_mu);
    return f;
}

/* Files in the folder the table does not know: left by an earlier boot. */
static void seg_forget_strays(void)
{
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/%s", kvm_storage_mount_point(), SEG_DIR);
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const unsigned long n = strtoul(e->d_name, NULL, 10);
        bool known = false;
        for (int i = 0; i < s_seg_count; i++) {
            known = known || s_segs[i].n == n;
        }
        if (!known && strstr(e->d_name, ".ts")) {
            char path[112];
            snprintf(path, sizeof(path), "%s/%.24s", dir, e->d_name);
            remove(path);
        }
    }
    closedir(d);
}

static void seg_pin(uint32_t first)
{
    for (int i = 0; i < SEG_PINS; i++) {
        if (!s_seg_pins[i]) {
            s_seg_pins[i] = first;
            return;
        }
    }
    s_seg_pins[0] = first; /* four clips at once: the oldest loses its hold */
}

void record_unpin_segments(uint32_t first)
{
    for (int i = 0; i < SEG_PINS; i++) {
        if (s_seg_pins[i] == first) {
            s_seg_pins[i] = 0;
            return;
        }
    }
}

size_t record_segment_path(uint32_t n, char *out, size_t cap)
{
    seg_path(n, out, cap);
    return strlen(out);
}

/*
 * The clip being saved is complete, or the writer is stopping with one half
 * made: hand its segments to the converter.
 */
static void seg_finish_event(uint32_t last)
{
    static record_finished_t done;
    memset(&done, 0, sizeof(done));
    portENTER_CRITICAL(&s_mu);
    done.event = true;
    done.segments = true;
    done.seg_first = s_event_first_seg;
    done.seg_last = last;
    done.chapters = s_chapter_count;
    memcpy(done.chapter, s_chapters, sizeof(s_chapters));
    for (int i = 0; i < s_seg_count; i++) {
        if (s_segs[i].n >= s_event_first_seg) {
            done.t0_us = s_segs[i].t0_us;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mu);
    snprintf(done.file, sizeof(done.file), "%s/%.40s.ts", KVM_RECORD_DIR, s_event_base);
    kvm_video_status_t vs;
    capture_status_get(&vs);
    done.width = vs.hres;
    done.height = vs.vres;
    done.parts = (int)(last - s_event_first_seg + 1);
    portENTER_CRITICAL(&s_mu);
    int64_t end_us = 0;
    for (int i = 0; i < s_seg_count; i++) {
        if (s_segs[i].n == last) {
            end_us = s_segs[i].t1_us;
        }
    }
    portEXIT_CRITICAL(&s_mu);
    write_clip_srt(done.file, done.t0_us, end_us ? end_us : esp_timer_get_time());
    s_event = false;
    s_event_end_us = 0;
    ESP_LOGI(TAG, "clip from segments %u..%u", (unsigned)done.seg_first, (unsigned)last);
    if (s_finished_cb) {
        s_finished_cb(&done);
    } else {
        record_unpin_segments(done.seg_first);
    }
}

/* The writer's hooks: a segment got its first frame; the one being written ended. */
static void seg_started(int64_t at_us)
{
    if (!s_background || !s_seg_count) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    if (!s_segs[s_seg_count - 1].t0_us) {
        s_segs[s_seg_count - 1].t0_us = at_us;
    }
    portEXIT_CRITICAL(&s_mu);
}

static void seg_ended(int64_t at_us, bool clip_done)
{
    if (!s_seg_count) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    seg_t *last = &s_segs[s_seg_count - 1];
    if (!last->t1_us) {
        last->t1_us = at_us;
    }
    const uint32_t n = last->n;
    portEXIT_CRITICAL(&s_mu);
    if (clip_done) {
        seg_finish_event(n);
    }
    seg_prune(at_us);
}

static FILE *open_part(char *rel, size_t rel_cap)
{
    if (s_background) {
        return seg_open(rel, rel_cap);
    }
    /* s_base is at most 47 characters; the part suffix and folder fit in 64. */
    if (s_part <= 1) {
        snprintf(rel, rel_cap, "%s/%.40s.ts", KVM_RECORD_DIR, s_base);
    } else {
        snprintf(rel, rel_cap, "%s/%.40s-part%d.ts", KVM_RECORD_DIR, s_base, s_part % 1000);
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), rel);
    FILE *f = fopen(path, "wb");
    if (f) {
        /* Our own 256 KB blocks go straight to the card, not through stdio's buffer. */
        setvbuf(f, NULL, _IONBF, 0);
    }
    return f;
}

static void close_file(void)
{
    write_chunk();
    if (s_file) {
        fflush(s_file);
        fsync(fileno(s_file));
        fclose(s_file);
        s_file = NULL;
    }
}

/*
 * The Opus packets that start up to @p until_us, at their times in this file;
 * any from before its first picture are left out. Waits up to @p wait_ms for
 * the encoder to get that far: where a file ends, its sound has to be in.
 */
static void write_audio(ts_mux_t *mux, int64_t until_us, uint32_t wait_ms)
{
    while (s_rec_audio) {
        if (!s_snd_held) {
            if (kvm_opus_rec_read(s_snd, s_snd_pkt, wait_ms) != ESP_OK) {
                return;
            }
            s_snd_held = true;
        }
        if (s_snd_pkt->start_us > until_us) {
            return;
        }
        s_snd_held = false;
        if (s_snd_pkt->start_us >= s_part_t0_us) {
            ts_mux_opus(mux, s_snd_pkt->data, s_snd_pkt->len,
                        (uint64_t)(s_snd_pkt->start_us - s_part_t0_us) * 9u / 100u);
        }
    }
}

/* Both under s_snd_mu. */
static void open_audio(bool dashcam)
{
    if (kvm_opus_rec_open(dashcam ? DASHCAM_AUDIO_S : 0, &s_snd) != ESP_OK) {
        return;
    }
    s_snd_pkt = heap_caps_malloc(sizeof(*s_snd_pkt), MALLOC_CAP_SPIRAM);
    if (!s_snd_pkt) {
        kvm_opus_rec_close(s_snd);
        s_snd = NULL;
        return;
    }
    s_snd_dashcam = dashcam;
}

static void close_audio(void)
{
    if (s_snd && !s_snd_dashcam) {
        ESP_LOGI(TAG, "sound: %u Opus packets lost", (unsigned)kvm_opus_rec_dropped(s_snd));
    }
    kvm_opus_rec_close(s_snd);
    s_snd = NULL;
    heap_caps_free(s_snd_pkt);
    s_snd_pkt = NULL;
    s_snd_held = false;
    s_snd_dashcam = false;
}

/* The writer is done with the sound: keep the dashcam's encoder, close the rest. */
static void writer_audio_done(void)
{
    xSemaphoreTake(s_snd_mu, portMAX_DELAY);
    s_writer_audio = false;
    s_rec_audio = false;
    if (!(s_dashcam && s_snd_dashcam)) {
        close_audio();
    }
    xSemaphoreGive(s_snd_mu);
}

void record_dashcam_audio(void)
{
    if (!s_snd_mu) {
        return;
    }
    kvm_audio_status_t a;
    kvm_audio_status(&a);
    const bool want = s_dashcam && a.enabled;
    xSemaphoreTake(s_snd_mu, portMAX_DELAY);
    if (want && !s_snd) {
        open_audio(true);
    } else if (!want && s_snd && !s_writer_audio) {
        close_audio();
    }
    xSemaphoreGive(s_snd_mu);
}

static void writer_task(void *arg)
{
    (void)arg;
    ts_mux_t mux;
    ts_mux_init(&mux, on_packet, NULL);
    mux.audio = s_rec_audio;
    int64_t t0_us = 0;
    int64_t last_us = 0; /* the last picture written */
    int64_t stop_at_us = 0; /* frames after this are not ours; 0 until a stop */
    int64_t synced_us = esp_timer_get_time();

    for (;;) {
        if (s_stop && !stop_at_us) {
            stop_at_us = esp_timer_get_time();
        }
        /* Where this recording ends: a stop, a length limit, or a clip's end time. */
        int64_t end_us = stop_at_us;
        const char *end_why = NULL;
        if (!end_us && s_event && s_event_end_us && !s_background) {
            end_us = s_event_end_us;
            end_why = "the clip is complete";
        } else if (!end_us && s_limit_us && t0_us) {
            end_us = t0_us + s_limit_us;
            end_why = s_limit_why;
        }

        frame_ring_entry_t e;
        xSemaphoreTake(s_ring_mu, portMAX_DELAY);
        bool have = frame_ring_peek(&s_ring, &e);
        if (have && end_us && e.at_us > end_us) {
            xSemaphoreGive(s_ring_mu);
            set_stopped(end_why ? end_why : "stopped");
            break; /* the rest stays in the ring: the dashcam's past */
        }
        if (have) {
            have = frame_ring_pop(&s_ring, s_frame, s_frame_cap, &e);
        }
        xSemaphoreGive(s_ring_mu);

        const int64_t now = esp_timer_get_time();
        if (!have) {
            /* Nothing queued. After a stop, a moment for the last frames to arrive. */
            if (stop_at_us && (!s_feed_running || now - stop_at_us > 1500000)) {
                break;
            }
            (void)xSemaphoreTake(s_ring_ready, pdMS_TO_TICKS(200));
        } else if (!s_write_failed) {
            /* A timelapse passes over everything but the keyframe it is due. The
             * due time moves on by the interval, not from the frame, so a GOP's
             * jitter does not slow the timelapse down. */
            bool take = true;
            if (s_every_us) {
                take = e.keyframe && e.at_us >= s_due_us; /* the rest is skipped, not lost */
                if (take) {
                    s_due_us = (s_due_us && e.at_us - s_due_us < s_every_us) ? s_due_us + s_every_us
                                                                           : e.at_us + s_every_us;
                }
            }
            if (take) {
                if (!t0_us) {
                    t0_us = e.at_us;
                }
                if (!s_part_t0_us) {
                    s_part_t0_us = e.at_us;
                    seg_started(e.at_us);
                }
                /* A clip on the card ends with the segment its end time falls in. */
                const bool clip_done = s_background && s_event && s_event_end_us &&
                                       e.at_us >= s_event_end_us;
                /* A new file on a keyframe, so each one plays on its own: when this
                 * one nears 4 GiB, or the split length the operator set. */
                const bool split = e.keyframe && e.at_us != s_part_t0_us &&
                                   (s_file_bytes >= PART_MAX_BYTES || clip_done ||
                                    (s_split_us && e.at_us - s_part_t0_us >= s_split_us));
                write_audio(&mux, split ? e.at_us - 1 : e.at_us, split ? 500 : 0);
                if (split) {
                    close_file();
                    if (s_background) {
                        seg_ended(e.at_us, clip_done);
                    }
                    s_part++;
                    s_file_bytes = 0;
                    char rel[64];
                    s_file = open_part(rel, sizeof(rel));
                    /* Times in the new file, and its subtitles, start from here. */
                    s_part_t0_us = e.at_us;
                    seg_started(e.at_us);
                    if (s_subs_on && !s_background) {
                        keylog_flush(&s_keylog);
                        close_srt();
                        if (s_file) {
                            open_srt(rel);
                        }
                    }
                    if (s_txt) {
                        close_txt();
                        if (s_file) {
                            open_txt(rel);
                        }
                    }
                    if (!s_file) {
                        s_write_failed = true;
                        set_stopped("could not start the next file on the card");
                        s_stop = true;
                    } else {
                        ts_mux_init(&mux, on_packet, NULL);
                        mux.audio = s_rec_audio;
                        s_tl_frame = 0;
                        portENTER_CRITICAL(&s_mu);
                        snprintf(s_st.file, sizeof(s_st.file), "%s", rel);
                        portEXIT_CRITICAL(&s_mu);
                        if (!s_background) {
                            ESP_LOGI(TAG, "continuing in %s", rel);
                        }
                    }
                }
                /* Each file's times start at zero, so every part plays from 0:00, and
                 * its subtitles line up; 90 kHz is the clock a transport stream uses. */
                const uint64_t pts = s_every_us ? (uint64_t)s_tl_frame++ * (90000u / TIMELAPSE_FPS)
                                                : (uint64_t)(e.at_us - s_part_t0_us) * 9u / 100u;
                ts_mux_frame(&mux, s_frame, e.len, pts, e.keyframe);
                last_us = e.at_us;
                portENTER_CRITICAL(&s_mu);
                s_st.frames++;
                portEXIT_CRITICAL(&s_mu);
            }
        }
        drain_key_events(now);
        text_tick(now);
        if (s_file && !s_write_failed && now - synced_us >= SYNC_US) {
            synced_us = now;
            write_chunk();
            fflush(s_file);
            fsync(fileno(s_file));
            if (s_srt) {
                fflush(s_srt);
                fsync(fileno(s_srt));
            }
            if (s_txt) {
                fflush(s_txt);
            }
        }
    }

    if (s_file && !s_write_failed && last_us) {
        write_audio(&mux, last_us, 500);
    }
    writer_audio_done();
    close_file();
    if (s_subs_on) {
        drain_key_events(INT64_MAX / 2);
        keylog_flush(&s_keylog);
        s_subs_on = false;
    }
    if (s_background) {
        seg_ended(esp_timer_get_time(), s_event);
        heap_caps_free(s_bg_cues);
        s_bg_cues = NULL;
        s_bg_cue_count = 0;
    }
    close_srt();
    close_txt();
    heap_caps_free(s_text_grid);
    heap_caps_free(s_text_buf);
    s_text_grid = NULL;
    s_text_buf = NULL;
    heap_caps_free(s_chunk);
    s_chunk = NULL;

    static record_finished_t done;
    memset(&done, 0, sizeof(done));
    portENTER_CRITICAL(&s_mu);
    s_st.recording = false;
    const kvm_record_status_t st = s_st;
    done.event = s_event;
    done.timelapse = s_every_us != 0;
    done.bytes = st.bytes;
    done.chapters = s_chapter_count;
    memcpy(done.chapter, s_chapters, sizeof(s_chapters));
    portEXIT_CRITICAL(&s_mu);
    snprintf(done.file, sizeof(done.file), "%s/%.40s.ts", KVM_RECORD_DIR, s_base);
    snprintf(done.stopped, sizeof(done.stopped), "%s", st.stopped);
    done.parts = s_part;
    done.t0_us = t0_us;
    kvm_video_status_t vs;
    capture_status_get(&vs);
    done.width = vs.hres;
    done.height = vs.vres;
    ESP_LOGI(TAG, "%s ended (%s): %s, %u frames, %llu bytes, %u dropped",
             s_background ? "dashcam writing" : "recording", s_background ? s_bg_stopped : st.stopped, st.file,
             (unsigned)st.frames, (unsigned long long)st.bytes, (unsigned)st.dropped);
    s_event = false;
    s_event_end_us = 0;
    const bool background = s_background;
    s_active = false;
    s_background = false;
    if (!s_dashcam) {
        /* Nobody needs the feed now; it frees the ring on its way out. */
        xSemaphoreGive(s_ring_ready);
    }
    if (s_finished_cb && st.frames && !background) {
        s_finished_cb(&done);
    }
    xSemaphoreGive(s_writer_done);
    vTaskDelete(NULL);
}

/* ---- control ------------------------------------------------------------------ */

static void on_card_leaving(const char *why)
{
    if (s_active) {
        stop_writer(why);
    }
}

/*
 * A codec wants the ring's PSRAM. The dashcam's ring is the largest thing this
 * component holds and the likeliest reason PSRAM has no long run left, so give
 * it back. After a real failure, stay out of the way for half a minute - but not
 * for a ring from the codec region, which breaks nothing up, and not when H.264
 * has only closed: the ring then raced the switch, and holding it back left
 * the next H.264 run with no dashcam (funcev, 2026-09-24). A recording in
 * progress keeps its ring - dropping that would lose the file being written,
 * and the codec switch that follows ends the recording cleanly anyway.
 */
static void on_memory_pressure(bool failed)
{
    if (s_active || !s_ring_buf) {
        return;
    }
    if (failed && !s_ring_lent) {
        s_ring_hold_until_us = esp_timer_get_time() + 30 * 1000000LL;
    }
    ring_free();
    ESP_LOGW(TAG, "gave the frame ring back: a codec needed the memory");
}

void kvm_record_init(void)
{
    if (!s_ctl) {
        s_ctl = xSemaphoreCreateMutex();
        s_writer_done = xSemaphoreCreateBinary();
        s_ring_mu = xSemaphoreCreateMutex();
        s_ring_ready = xSemaphoreCreateBinary();
        s_keylog_mu = xSemaphoreCreateMutex();
        s_snd_mu = xSemaphoreCreateMutex();
    }
    kvm_storage_set_fs_leaving_cb(on_card_leaving);
    capture_set_memory_pressure_cb(on_memory_pressure);
    if (!s_key_events) {
        s_key_events = xQueueCreateWithCaps(KEY_EVENTS, sizeof(key_event_t), MALLOC_CAP_SPIRAM);
    }
    usb_hid_set_observer(on_hid_report);
}

const char *kvm_record_blocked(void)
{
    if (s_background && s_event) {
        return "the dashcam is saving a clip; try again in a moment";
    }
    if (s_active && !s_background) {
        return "a recording is already running";
    }
    if (!kvm_storage_writable()) {
        return kvm_storage_write_unavailable_reason();
    }
    if (video_frame_payload() != VIDEO_PAYLOAD_H264) {
        return "recording needs the H.264 codec";
    }
    return NULL;
}

/*
 * The shared start. @p event_title is NULL for an ordinary recording, which
 * starts from now; a clip keeps the last @p pre_s seconds the ring holds.
 */
static esp_err_t start(uint32_t max_seconds, const char *event_title, uint32_t pre_s,
                       uint32_t post_s, uint32_t every_s, bool background, char *why, size_t why_cap)
{
    if (!s_ctl) {
        snprintf(why, why_cap, "the recorder is not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    const char *blocked = kvm_record_blocked();
    if (!blocked && background && s_active) {
        blocked = "the recorder is busy";
    }
    if (blocked) {
        snprintf(why, why_cap, "%s", blocked);
        xSemaphoreGive(s_ctl);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_background) {
        /* The operator's recording takes the writer; the dashcam picks up again
         * after it. */
        stop_writer("a recording started");
    }
    if (card_free_bytes() < MIN_FREE_BYTES) {
        snprintf(why, why_cap, "the card is full");
        xSemaphoreGive(s_ctl);
        return ESP_ERR_NO_MEM;
    }
    char rel[64];
    char stamp_base[48];
    if (background) {
        if (!ensure_dir(KVM_RECORD_DIR) || !ensure_dir(SEG_DIR)) {
            snprintf(why, why_cap, "could not create the %s folder on the card", SEG_DIR);
            xSemaphoreGive(s_ctl);
            return ESP_FAIL;
        }
        seg_forget_strays();
        snprintf(stamp_base, sizeof(stamp_base), "dashcam");
    } else if (!ensure_dir(KVM_RECORD_DIR) ||
        !unique_name(KVM_RECORD_DIR, "ts", stamp_base, sizeof(stamp_base), rel, sizeof(rel))) {
        snprintf(why, why_cap, "could not create the %s folder on the card", KVM_RECORD_DIR);
        xSemaphoreGive(s_ctl);
        return ESP_FAIL;
    }
    /* A clip says what it is in its name. */
    if (event_title) {
        snprintf(s_base, sizeof(s_base), "%.36s-event", stamp_base);
    } else if (every_s) {
        snprintf(s_base, sizeof(s_base), "%.36s-timelapse", stamp_base);
    } else {
        snprintf(s_base, sizeof(s_base), "%s", stamp_base);
    }
    s_part = 1;
    s_file_bytes = 0;
    s_chunk_len = 0;
    s_write_failed = false;
    s_chunk = heap_caps_aligned_alloc(64, WRITE_CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_chunk || !feed_ensure()) {
        heap_caps_free(s_chunk);
        s_chunk = NULL;
        snprintf(why, why_cap, "not enough memory to record");
        xSemaphoreGive(s_ctl);
        return ESP_ERR_NO_MEM;
    }
    s_background = background;
    if (background) {
        s_bg_stopped[0] = '\0';
    }
    s_file = open_part(rel, sizeof(rel));
    if (!s_file) {
        s_background = false;
        heap_caps_free(s_chunk);
        s_chunk = NULL;
        snprintf(why, why_cap, "could not create the file on the card");
        xSemaphoreGive(s_ctl);
        return ESP_FAIL;
    }

    /* Sound when audio is on and working; without it the recording goes on
     * silent. A clip takes the dashcam's encoder with the past it holds. */
    xSemaphoreTake(s_snd_mu, portMAX_DELAY);
    if (!every_s && !s_snd) {
        open_audio(false);
    }
    s_rec_audio = !every_s && s_snd;
    s_writer_audio = true;
    xSemaphoreGive(s_snd_mu);

    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    if (event_title) {
        /* Keep the last pre_s seconds before now, a whole group at a time, and
         * begin on a keyframe: the ring may still start inside a group the last
         * recording left behind. */
        frame_ring_entry_t oldest;
        while (frame_ring_peek(&s_ring, &oldest) &&
               (!oldest.keyframe || now - oldest.at_us > (int64_t)pre_s * 1000000)) {
            frame_ring_drop_group(&s_ring);
        }
    } else {
        /* An ordinary recording starts now, not with the dashcam's past. */
        frame_ring_clear(&s_ring);
        s_feed_resync = true;
    }
    frame_ring_entry_t first;
    const bool have_past = frame_ring_peek(&s_ring, &first);
    xSemaphoreGive(s_ring_mu);

    /* Settings are read once: a recording keeps the choice it started with. */
    s_limit_us = 0;
    s_limit_why[0] = '\0';
    s_every_us = (int64_t)every_s * 1000000;
    s_due_us = 0;
    s_tl_frame = 0;
    if (!event_title) {
        if (max_seconds) {
            s_limit_us = (int64_t)max_seconds * 1000000;
            snprintf(s_limit_why, sizeof(s_limit_why), "it reached the %lu s it was started for",
                     (unsigned long)max_seconds);
        } else if (!every_s) {
            /* A timelapse is meant to run for hours: only a length it is given. */
            const int32_t max_min = kvm_setting_int("rec_max_min");
            s_limit_us = (int64_t)max_min * 60 * 1000000;
            snprintf(s_limit_why, sizeof(s_limit_why), "it reached the %ld-minute limit", (long)max_min);
        }
    }
    /* A clip is short and is one file, whatever the split length says. */
    s_split_us = background              ? SEG_US
                 : (event_title || every_s) ? 0
                                            : (int64_t)kvm_setting_int("rec_split_min") * 60 * 1000000;
    if (background) {
        s_limit_us = 0;
    }
    const keylog_mode_t subs = (keylog_mode_t)kvm_setting_int("rec_subs");
    s_part_t0_us = 0;
    s_subs_on = false;
    /* A timelapse's clock is not the wall clock, so subtitles would not line up. */
    if (subs != KEYLOG_OFF && s_key_events && !every_s && background) {
        /* The dashcam keeps its cues in memory; a clip writes its own .srt. */
        if (!s_bg_cues) {
            s_bg_cues = heap_caps_calloc(BG_CUES, sizeof(bg_cue_t), MALLOC_CAP_SPIRAM);
            s_bg_cue_head = 0;
            s_bg_cue_count = 0;
        }
        s_subs_on = s_bg_cues && keylog_begin();
    } else if (subs != KEYLOG_OFF && s_key_events && !every_s) {
        /* A clip keeps its own cue builder: the one the dashcam was running
         * holds the past, and that past is written into the file below. */
        const bool keep_past = event_title && s_cues_on;
        if (!keep_past) {
            (void)keylog_begin();
        }
        open_srt(rel);
        s_subs_on = s_srt != NULL;
        if (s_subs_on && keep_past && have_past) {
            /* What was pressed before the button, timed from the clip's first
             * frame like everything else in the file. */
            s_part_t0_us = first.at_us;
            replay_cues(first.at_us);
        }
    }

    /* The dashcam's own writing is not indexed: a clip is cut from it later, and
     * the text would have to be cut with it. */
    if (!background && kvm_setting_bool("rec_text")) {
        s_text_grid = heap_caps_malloc(sizeof(*s_text_grid), MALLOC_CAP_SPIRAM);
        s_text_buf = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
        if (s_text_grid && s_text_buf) {
            open_txt(rel);
        }
    }

    portENTER_CRITICAL(&s_mu);
    if (!background) {
        memset(&s_st, 0, sizeof(s_st));
        s_st.recording = true;
        s_st.timelapse_every = every_s;
        s_st.audio = s_rec_audio;
        snprintf(s_st.file, sizeof(s_st.file), "%s", rel);
        s_started_us = now;
    }
    s_chapter_count = 0;
    if (event_title) {
        if (have_past && first.at_us < now) {
            s_chapters[s_chapter_count].at_us = first.at_us;
            snprintf(s_chapters[s_chapter_count].title, RECORD_CHAPTER_TITLE, "Before");
            s_chapter_count++;
        }
        s_chapters[s_chapter_count].at_us = now;
        snprintf(s_chapters[s_chapter_count].title, RECORD_CHAPTER_TITLE, "%s", event_title);
        s_chapter_count++;
    }
    portEXIT_CRITICAL(&s_mu);
    s_event = event_title != NULL;
    s_event_started_us = now;
    s_event_end_us = event_title ? now + (int64_t)post_s * 1000000 : 0;
    s_stop = false;
    (void)xSemaphoreTake(s_writer_done, 0);
    s_active = true;
    if (xTaskCreate(writer_task, "kvm_rec_wr", 4096, NULL, WRITER_PRIO, NULL) != pdPASS) {
        fclose(s_file);
        s_file = NULL;
        heap_caps_free(s_chunk);
        s_chunk = NULL;
        writer_audio_done();
        s_active = false;
        s_background = false;
        s_event = false;
        s_st.recording = background ? s_st.recording : false;
        s_subs_on = false;
        close_srt();
        close_txt();
        snprintf(why, why_cap, "could not start the recorder");
        xSemaphoreGive(s_ctl);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s to %s",
             background ? "dashcam" : event_title ? "saving a clip" : every_s ? "timelapse" : "recording", rel);
    xSemaphoreGive(s_ctl);
    return ESP_OK;
}

esp_err_t kvm_record_start(uint32_t max_seconds, char *why, size_t why_cap)
{
    return start(max_seconds, NULL, 0, 0, 0, false, why, why_cap);
}

esp_err_t kvm_record_start_timelapse(uint32_t every_s, uint32_t max_seconds, char *why, size_t why_cap)
{
    if (every_s < 1 || every_s > KVM_TIMELAPSE_EVERY_MAX) {
        snprintf(why, why_cap, "a timelapse wants a frame every 1..%u seconds", KVM_TIMELAPSE_EVERY_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    return start(max_seconds, NULL, 0, 0, every_s, false, why, why_cap);
}

static void stop_writer(const char *reason)
{
    if (!s_active) {
        return;
    }
    set_stopped(reason ? reason : "stopped");
    s_stop = true;
    xSemaphoreGive(s_ring_ready);
    /* The ring drains first, so a stop never throws away what was captured.
     * At the card's speed that is a second or two at most. */
    if (xSemaphoreTake(s_writer_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "the recording did not close in time");
    }
}

void kvm_record_stop(const char *reason)
{
    /* The operator stops their own recording, never the dashcam's writing. */
    if (!s_background) {
        stop_writer(reason);
    }
}

esp_err_t record_start_segments(char *why, size_t why_cap)
{
    return start(0, NULL, 0, 0, 0, true, why, why_cap);
}

void record_stop_segments(const char *reason, bool forget)
{
    if (s_ctl) {
        xSemaphoreTake(s_ctl, portMAX_DELAY);
    }
    if (s_background) {
        stop_writer(reason);
    }
    if (forget && !s_active) {
        /* Switched off: the past on the card goes too. */
        while (s_seg_count > 0) {
            seg_drop_oldest();
        }
        seg_forget_strays();
    }
    if (s_ctl) {
        xSemaphoreGive(s_ctl);
    }
}

bool record_segments_running(void)
{
    return s_background;
}

/* A clip made of segments: pin the ones reaching back pre_s, end post_s from now. */
static esp_err_t seg_mark_event(const char *title, uint32_t pre_s, uint32_t post_s, char *why, size_t why_cap)
{
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    if (!s_active || !s_background || !s_seg_count) {
        snprintf(why, why_cap, "the dashcam is not writing to the card");
        xSemaphoreGive(s_ctl);
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now = esp_timer_get_time();
    const int64_t from = now - (int64_t)pre_s * 1000000;
    portENTER_CRITICAL(&s_mu);
    int first = 0;
    for (int i = 0; i < s_seg_count; i++) {
        if (s_segs[i].t0_us && s_segs[i].t0_us <= from) {
            first = i;
        }
    }
    const uint32_t first_n = s_segs[first].n;
    const int64_t first_t0 = s_segs[first].t0_us;
    s_chapter_count = 0;
    if (first_t0 && first_t0 < now) {
        s_chapters[s_chapter_count].at_us = first_t0;
        snprintf(s_chapters[s_chapter_count].title, RECORD_CHAPTER_TITLE, "Before");
        s_chapter_count++;
    }
    s_chapters[s_chapter_count].at_us = now;
    snprintf(s_chapters[s_chapter_count].title, RECORD_CHAPTER_TITLE, "%s", title);
    s_chapter_count++;
    portEXIT_CRITICAL(&s_mu);

    char base[48], rel[64];
    if (!unique_name(KVM_RECORD_DIR, "mp4", base, sizeof(base), rel, sizeof(rel))) {
        snprintf(base, sizeof(base), "clip");
    }
    snprintf(s_event_base, sizeof(s_event_base), "%.36s-event", base);
    seg_pin(first_n);
    s_event_first_seg = first_n;
    s_event_started_us = now;
    s_event_end_us = now + (int64_t)post_s * 1000000;
    s_event = true;
    ESP_LOGI(TAG, "saving a clip from the card: %s (from segment %u)", s_event_base, (unsigned)first_n);
    xSemaphoreGive(s_ctl);
    return ESP_OK;
}

bool kvm_record_active(void)
{
    return s_active && !s_background;
}

void kvm_record_status(kvm_record_status_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    *out = s_st;
    if (s_st.recording) {
        out->seconds = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
    }
    portEXIT_CRITICAL(&s_mu);
    out->event = s_event;
    out->dashcam = s_dashcam || s_background;
    const int64_t left = s_event && s_event_end_us ? s_event_end_us - esp_timer_get_time() : 0;
    out->clip_seconds_left = left > 0 ? (uint32_t)((left + 999999) / 1000000) : 0;
    out->clips_converting = dashcam_clips_converting();
    dashcam_last_clip(out->last_clip, sizeof(out->last_clip));
    if (s_background && s_event) {
        snprintf(out->file, sizeof(out->file), "%s/%.40s.mp4", KVM_RECORD_DIR, s_event_base);
    }
    out->dashcam_no_memory = s_dashcam_no_memory;
    out->preroll_seconds = record_preroll_seconds();
}

/* ---- for the dashcam ------------------------------------------------------------ */

void record_set_dashcam(bool on)
{
    if (!s_ctl) {
        return;
    }
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    s_dashcam = on;
    s_dashcam_no_memory = false;
    /* Keystrokes are part of the past too: start building cues now, not when
     * the clip starts, or a clip's subtitles would begin at the button. */
    if (on && !s_cues_on && !s_active) {
        if (!s_bg_cues) {
            s_bg_cues = heap_caps_calloc(BG_CUES, sizeof(bg_cue_t), MALLOC_CAP_SPIRAM);
            s_bg_cue_head = 0;
            s_bg_cue_count = 0;
        }
        s_cues_on = s_bg_cues && keylog_begin();
    } else if (!on && !s_active) {
        s_cues_on = false;
        heap_caps_free(s_bg_cues);
        s_bg_cues = NULL;
        s_bg_cue_count = 0;
        s_bg_cue_head = 0;
    }
    if (on && !s_feed_running) {
        /* The feed takes the ring itself, once H.264 runs. */
        s_feed_running = true;
        if (xTaskCreate(feed_task, "kvm_rec_feed", 4096, NULL, FEED_PRIO, NULL) != pdPASS) {
            s_feed_running = false;
            s_dashcam = false;
        }
    }
    const bool release = !on && !s_active;
    xSemaphoreGive(s_ctl);
    record_dashcam_audio();
    if (release) {
        /* Now, not when the feed next wakes: a codec switch may need the memory. */
        ring_free();
        xSemaphoreGive(s_ring_ready);
    }
}

uint32_t record_preroll_seconds(void)
{
    if (s_background) {
        portENTER_CRITICAL(&s_mu);
        const int64_t t0 = s_seg_count ? s_segs[0].t0_us : 0;
        portEXIT_CRITICAL(&s_mu);
        const int64_t span = t0 ? (esp_timer_get_time() - t0) / 1000000 : 0;
        const int64_t cap = kvm_setting_int("dashcam_pre_s");
        return (uint32_t)(span < cap ? span : cap);
    }
    if (!s_ring_mu || !s_ring_buf) {
        return 0;
    }
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    const int64_t span = s_ring_buf ? frame_ring_span_us(&s_ring) : 0;
    xSemaphoreGive(s_ring_mu);
    return (uint32_t)(span / 1000000);
}

esp_err_t record_start_event(const char *title, uint32_t pre_s, uint32_t post_s, char *why,
                             size_t why_cap)
{
    if (s_background) {
        return seg_mark_event(title, pre_s, post_s, why, why_cap);
    }
    return start(0, title, pre_s, post_s, 0, false, why, why_cap);
}

void record_extend_event(int64_t end_us)
{
    if (!s_event) {
        return;
    }
    const int64_t cap = s_event_started_us + EVENT_MAX_US;
    if (end_us > cap) {
        end_us = cap;
    }
    if (end_us > s_event_end_us) {
        s_event_end_us = end_us;
    }
}

bool record_ring_idle(void)
{
    return s_ring_buf == NULL || s_dashcam || s_background;
}

bool record_event_running(void)
{
    return s_active && s_event;
}

void record_add_chapter(int64_t at_us, const char *title)
{
    portENTER_CRITICAL(&s_mu);
    if (s_event && s_chapter_count < RECORD_CHAPTERS_MAX) {
        s_chapters[s_chapter_count].at_us = at_us;
        snprintf(s_chapters[s_chapter_count].title, RECORD_CHAPTER_TITLE, "%s", title);
        s_chapter_count++;
    }
    portEXIT_CRITICAL(&s_mu);
}

void record_set_finished_cb(void (*cb)(const record_finished_t *done))
{
    s_finished_cb = cb;
}

/* ---- screenshots ------------------------------------------------------------- */

const char *kvm_record_screenshot_blocked(void)
{
    if (!kvm_storage_writable()) {
        return kvm_storage_write_unavailable_reason();
    }
    kvm_video_status_t vs;
    capture_status_get(&vs);
    if (!vs.signal) {
        return "there is no video signal";
    }
    return NULL;
}

esp_err_t kvm_record_screenshot(char *file, size_t file_cap, char *why, size_t why_cap)
{
    const char *blocked = kvm_record_screenshot_blocked();
    if (blocked) {
        snprintf(why, why_cap, "%s", blocked);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *jpeg = NULL;
    size_t len = 0;
    esp_err_t err = capture_snapshot_jpeg(&jpeg, &len, 3000);
    if (err != ESP_OK) {
        snprintf(why, why_cap, "could not take the picture (%s)", esp_err_to_name(err));
        return err;
    }
    char base[40], rel[64];
    if (!ensure_dir(KVM_SCREENSHOT_DIR) ||
        !unique_name(KVM_SCREENSHOT_DIR, "jpg", base, sizeof(base), rel, sizeof(rel))) {
        free(jpeg);
        snprintf(why, why_cap, "could not create the %s folder on the card", KVM_SCREENSHOT_DIR);
        return ESP_FAIL;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), rel);
    FILE *f = fopen(path, "wb");
    const bool ok = f && fwrite(jpeg, 1, len, f) == len;
    if (f) {
        fclose(f);
    }
    free(jpeg);
    if (!ok) {
        remove(path);
        snprintf(why, why_cap, "could not write the file to the card");
        return ESP_FAIL;
    }
    snprintf(file, file_cap, "%s", rel);
    ESP_LOGI(TAG, "screenshot: %s, %zu bytes", rel, len);
    kvm_upload_auto(KVM_UPLOAD_SCREENSHOT, rel);
    return ESP_OK;
}
