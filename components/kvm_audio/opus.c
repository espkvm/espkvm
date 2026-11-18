/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_audio.h. One Opus encoder for everything: the browser's readers and
 * the recorder. It runs while either is open, holds one PCM reader, brings the
 * sound to 48 kHz (Opus takes nothing in between) and encodes 20 ms at a time.
 *
 * The task keeps a count of the 48 kHz frames it has put out since it opened,
 * which is also a clock: frame n is due n / 48000 s after the start. Each block
 * of sound goes where its capture time says. A little early or late is taken
 * as it comes; further off, the stream is filled with silence up to it or the
 * overlap is cut. With no sound at all, silence keeps the count up with the
 * time; a packet of it is one made once at the start, not encoded again.
 *
 * The browser gets only the packets made from sound, as they come: a packet
 * after a gap or after silence says so (resync). The recorder gets every
 * packet, silence included, through a ring buffer in PSRAM, so one stuck on a
 * slow card loses nothing for a few seconds, and the dashcam keeps its minutes
 * of past there.
 */
#include <stdlib.h>
#include <string.h>

#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_priv.h"
#include "kvm_audio.h"

static const char *TAG = "opus";

#define MAX_READERS 4
#define COMPLEXITY 1
/* 500 ms of 5 ms blocks: the encoder sits below the video and can wait
   that long behind it. */
#define PCM_DEPTH 100
/* libopus keeps its work arrays on the stack: it used 36 KB at 96 kbit/s. */
#define TASK_STACK (48 * 1024)
#define TASK_PRIO 4
/* Silence is made up only this far behind now: sound may still be on its way. */
#define LAG_US 400000
/* 20 ms: closer than this a block is taken where it falls. */
#define SLACK (OUT_RATE / 50)
/* The recorder's ring holds at least this much: about 8 s. */
#define REC_MIN_BYTES (128 * 1024)
/* Room a second of packets takes in the ring: the bitrate with a margin for
 * the louder parts, and each item's header. */
#define REC_BYTES_PER_S (KVM_OPUS_BITRATE / 8 * 5 / 4 + 50 * 32)

struct kvm_opus_reader {
    QueueHandle_t queue;
    StaticQueue_t queue_state;
    uint8_t *storage;
};

typedef struct {
    int64_t start_us;
    uint32_t ts;
    uint8_t data[];
} item_t;

struct kvm_opus_rec {
    RingbufHandle_t out;
    volatile uint32_t dropped;
};

/* Large, so in PSRAM, owned by the task. */
typedef struct {
    void *enc;
    int64_t origin_us;
    uint32_t origin_ts;
    uint64_t written;  /* 48 kHz frames since origin_us */
    uint64_t packets;  /* packets made */
    unsigned have;     /* frames in pcm */
    bool live_gap;     /* the browser's next packet starts after a gap */
    uint8_t silence[KVM_OPUS_MAX];
    uint16_t silence_len;
    kvm_audio_block_t in;
    int16_t pcm48[RESAMPLE_MAX * KVM_AUDIO_CHANNELS];
    int16_t pcm[KVM_OPUS_FRAMES * KVM_AUDIO_CHANNELS];
    kvm_opus_packet_t pkt;
    kvm_opus_packet_t old;
    uint8_t item[sizeof(item_t) + KVM_OPUS_MAX];
} work_t;

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_task;
static kvm_opus_reader_t *s_readers[MAX_READERS];
static kvm_opus_rec_t *s_rec;

static bool wanted(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool any = s_rec != NULL;
    for (int i = 0; i < MAX_READERS; i++) {
        any |= s_readers[i] != NULL;
    }
    xSemaphoreGive(s_mu);
    return any;
}

/* The packet in w->pkt, made from sound (@p heard) or silence, to whoever wants it. */
static void deliver(work_t *w, bool heard)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (heard) {
        w->pkt.resync = w->live_gap;
        w->live_gap = false;
        for (int i = 0; i < MAX_READERS; i++) {
            kvm_opus_reader_t *r = s_readers[i];
            if (!r) {
                continue;
            }
            if (xQueueSend(r->queue, &w->pkt, 0) != pdTRUE) {
                (void)xQueueReceive(r->queue, &w->old, 0);
                (void)xQueueSend(r->queue, &w->pkt, 0);
            }
        }
    } else {
        w->live_gap = true;
    }
    kvm_opus_rec_t *rec = s_rec;
    if (rec) {
        item_t *it = (item_t *)w->item;
        it->start_us = w->pkt.start_us;
        it->ts = w->pkt.ts;
        memcpy(it->data, w->pkt.data, w->pkt.len);
        /* Full: the oldest packets make room. */
        while (xRingbufferSend(rec->out, it, sizeof(item_t) + w->pkt.len, 0) != pdTRUE) {
            size_t size = 0;
            void *old = xRingbufferReceive(rec->out, &size, 0);
            rec->dropped++;
            if (!old) {
                break;
            }
            vRingbufferReturnItem(rec->out, old);
        }
    }
    xSemaphoreGive(s_mu);
}

static void stamp(work_t *w)
{
    w->pkt.start_us = w->origin_us + (int64_t)(w->packets * KVM_OPUS_FRAMES * 1000000 / OUT_RATE);
    w->pkt.ts = w->origin_ts + (uint32_t)(w->packets * KVM_OPUS_FRAMES);
    w->packets++;
}

static void encode(work_t *w, bool heard)
{
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)w->pcm, .len = sizeof(w->pcm)};
    esp_audio_enc_out_frame_t out = {.buffer = w->pkt.data, .len = KVM_OPUS_MAX};
    stamp(w);
    if (esp_opus_enc_process(w->enc, &in, &out) != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0 ||
        out.encoded_bytes > KVM_OPUS_MAX) {
        w->live_gap = true;
        return;
    }
    w->pkt.len = (uint16_t)out.encoded_bytes;
    deliver(w, heard);
}

/* @p n frames of @p pcm (48 kHz stereo), or of silence when it is NULL. */
static void put(work_t *w, const int16_t *pcm, size_t n)
{
    while (n) {
        if (!pcm && w->silence_len && w->have == 0 && n >= KVM_OPUS_FRAMES) {
            stamp(w);
            memcpy(w->pkt.data, w->silence, w->silence_len);
            w->pkt.len = w->silence_len;
            deliver(w, false);
            w->written += KVM_OPUS_FRAMES;
            n -= KVM_OPUS_FRAMES;
            continue;
        }
        size_t k = KVM_OPUS_FRAMES - w->have;
        if (k > n) {
            k = n;
        }
        int16_t *dst = &w->pcm[w->have * KVM_AUDIO_CHANNELS];
        if (pcm) {
            memcpy(dst, pcm, k * KVM_AUDIO_CHANNELS * sizeof(int16_t));
            pcm += k * KVM_AUDIO_CHANNELS;
        } else {
            memset(dst, 0, k * KVM_AUDIO_CHANNELS * sizeof(int16_t));
        }
        w->have += (unsigned)k;
        w->written += k;
        n -= k;
        if (w->have == KVM_OPUS_FRAMES) {
            /* A packet partly made up is still sound: the browser plays it. */
            encode(w, pcm != NULL);
            w->have = 0;
        }
    }
}

static int64_t frames_at(const work_t *w, int64_t us)
{
    return (us - w->origin_us) * OUT_RATE / 1000000;
}

static void enc_config(esp_opus_enc_config_t *cfg)
{
    *cfg = (esp_opus_enc_config_t)ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg->sample_rate = OUT_RATE;
    cfg->channel = KVM_AUDIO_CHANNELS;
    cfg->bitrate = KVM_OPUS_BITRATE;
    cfg->application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    cfg->complexity = COMPLEXITY;
    cfg->enable_vbr = true;
}

/*
 * One silent packet, from an encoder of its own so the stream's is left as it
 * is. The first packets out of an encoder carry its start-up; the third is a
 * steady one.
 */
static void make_silence(work_t *w)
{
    esp_opus_enc_config_t cfg;
    enc_config(&cfg);
    void *enc = NULL;
    w->silence_len = 0;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK) {
        return;
    }
    memset(w->pcm, 0, sizeof(w->pcm));
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)w->pcm, .len = sizeof(w->pcm)};
    esp_audio_enc_out_frame_t out = {.buffer = w->silence, .len = sizeof(w->silence)};
    bool ok = false;
    for (int i = 0; i < 3; i++) {
        out.encoded_bytes = 0;
        ok = esp_opus_enc_process(enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0 &&
             out.encoded_bytes <= KVM_OPUS_MAX;
    }
    esp_opus_enc_close(enc);
    if (ok) {
        w->silence_len = (uint16_t)out.encoded_bytes;
    }
}

static void encode_task(void *arg)
{
    (void)arg;
    work_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) {
        ESP_LOGE(TAG, "no memory");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        if (!wanted()) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        kvm_audio_reader_t *pcm = NULL;
        if (kvm_audio_open(PCM_DEPTH, &pcm) != ESP_OK) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        esp_opus_enc_config_t cfg;
        enc_config(&cfg);
        if (esp_opus_enc_open(&cfg, sizeof(cfg), &w->enc) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "encoder did not open");
            kvm_audio_close(pcm);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        make_silence(w);
        if (!w->silence_len) {
            ESP_LOGW(TAG, "no ready silent packet, silence is encoded");
        }
        ESP_LOGI(TAG, "encoding at %d bit/s", KVM_OPUS_BITRATE);

        resampler_t rs = {0};
        uint64_t next_index = 0;
        uint32_t epoch = 0;
        w->origin_us = esp_timer_get_time();
        w->origin_ts = (uint32_t)(w->origin_us * 48 / 1000);
        w->written = 0;
        w->packets = 0;
        w->have = 0;
        w->live_gap = true;
        while (wanted()) {
            if (kvm_audio_read(pcm, &w->in, 50) != ESP_OK) {
                const int64_t due = frames_at(w, esp_timer_get_time() - LAG_US);
                if (due > (int64_t)w->written) {
                    put(w, NULL, (size_t)(due - (int64_t)w->written));
                }
                continue;
            }
            const kvm_audio_block_t *b = &w->in;
            if (b->rate != rs.rate || b->epoch != epoch || b->frame_index != next_index) {
                resample_reset(&rs, b->rate);
                epoch = b->epoch;
            }
            next_index = b->frame_index + b->frames;
            int64_t at_us;
            const size_t n = resample_block(&rs, b, w->pcm48, &at_us);
            const int64_t due = frames_at(w, at_us);
            const int64_t pos = (int64_t)w->written;
            size_t skip = 0;
            if (due > pos + SLACK) {
                put(w, NULL, (size_t)(due - pos)); /* a gap */
            } else if (due < pos - SLACK) {
                skip = (size_t)(pos - due) < n ? (size_t)(pos - due) : n; /* already filled */
            }
            put(w, w->pcm48 + skip * KVM_AUDIO_CHANNELS, n - skip);
        }
        esp_opus_enc_close(w->enc);
        w->enc = NULL;
        kvm_audio_close(pcm);
        ESP_LOGI(TAG, "encoder stopped after %llu packets; stack never below %u bytes free",
                 (unsigned long long)w->packets, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

esp_err_t opus_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    return s_mu ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Start the task if it is not running, and wake it. Called with s_mu held. */
static esp_err_t wake_locked(void)
{
    if (!s_task && xTaskCreateWithCaps(encode_task, "opus", TASK_STACK, NULL, TASK_PRIO, &s_task,
                                       MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

static bool audio_on(void)
{
    kvm_audio_status_t st;
    kvm_audio_status(&st);
    return st.enabled;
}

esp_err_t kvm_opus_open(uint16_t depth, kvm_opus_reader_t **out)
{
    *out = NULL;
    if (depth == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mu || !audio_on()) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_opus_reader_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    r->storage = heap_caps_malloc((size_t)depth * sizeof(kvm_opus_packet_t), MALLOC_CAP_SPIRAM);
    if (!r->storage) {
        free(r);
        return ESP_ERR_NO_MEM;
    }
    r->queue = xQueueCreateStatic(depth, sizeof(kvm_opus_packet_t), r->storage, &r->queue_state);

    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; i++) {
        if (!s_readers[i]) {
            s_readers[i] = r;
            err = ESP_OK;
            break;
        }
    }
    if (err == ESP_OK) {
        err = wake_locked();
        if (err != ESP_OK) {
            for (int i = 0; i < MAX_READERS; i++) {
                if (s_readers[i] == r) {
                    s_readers[i] = NULL;
                }
            }
        }
    }
    xSemaphoreGive(s_mu);
    if (err != ESP_OK) {
        vQueueDelete(r->queue);
        free(r->storage);
        free(r);
        return err;
    }
    *out = r;
    return ESP_OK;
}

esp_err_t kvm_opus_read(kvm_opus_reader_t *r, kvm_opus_packet_t *pkt, uint32_t timeout_ms)
{
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && t == 0) {
        t = 1;
    }
    return xQueueReceive(r->queue, pkt, t) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void kvm_opus_close(kvm_opus_reader_t *r)
{
    if (!r) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; i++) {
        if (s_readers[i] == r) {
            s_readers[i] = NULL;
        }
    }
    xSemaphoreGive(s_mu);
    vQueueDelete(r->queue);
    free(r->storage);
    free(r);
}

esp_err_t kvm_opus_rec_open(uint32_t keep_s, kvm_opus_rec_t **out)
{
    *out = NULL;
    if (!s_mu || !audio_on()) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_opus_rec_t *rec = heap_caps_calloc(1, sizeof(*rec), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!rec) {
        return ESP_ERR_NO_MEM;
    }
    const size_t bytes = (size_t)keep_s * REC_BYTES_PER_S > REC_MIN_BYTES
                             ? (size_t)keep_s * REC_BYTES_PER_S
                             : REC_MIN_BYTES;
    rec->out = xRingbufferCreateWithCaps(bytes, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    if (!rec->out) {
        free(rec);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_rec) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_rec = rec;
        err = wake_locked();
        if (err != ESP_OK) {
            s_rec = NULL;
        }
    }
    xSemaphoreGive(s_mu);
    if (err != ESP_OK) {
        vRingbufferDeleteWithCaps(rec->out);
        free(rec);
        return err;
    }
    *out = rec;
    return ESP_OK;
}

esp_err_t kvm_opus_rec_read(kvm_opus_rec_t *rec, kvm_opus_packet_t *pkt, uint32_t timeout_ms)
{
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && t == 0) {
        t = 1;
    }
    size_t size = 0;
    item_t *it = xRingbufferReceive(rec->out, &size, t);
    if (!it) {
        return ESP_ERR_TIMEOUT;
    }
    pkt->start_us = it->start_us;
    pkt->ts = it->ts;
    pkt->resync = false;
    pkt->len = (uint16_t)(size - sizeof(item_t));
    memcpy(pkt->data, it->data, pkt->len);
    vRingbufferReturnItem(rec->out, it);
    return ESP_OK;
}

uint32_t kvm_opus_rec_dropped(const kvm_opus_rec_t *rec)
{
    return rec->dropped;
}

void kvm_opus_rec_close(kvm_opus_rec_t *rec)
{
    if (!rec) {
        return;
    }
    /* Under the lock: the task writes to the ring only while holding it. */
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_rec == rec) {
        s_rec = NULL;
    }
    xSemaphoreGive(s_mu);
    vRingbufferDeleteWithCaps(rec->out);
    free(rec);
}
