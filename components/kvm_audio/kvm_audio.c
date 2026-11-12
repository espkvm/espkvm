/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_priv.h"
#include "capture.h"
#include "kvm_caps.h"
#include "kvm_settings.h"

static const char *TAG = "audio";

#define MAX_READERS 4
#define DMA_BUFFERS 8
#define STAMPS 16
#define TASK_STACK 4096
/* Above lwIP (18): it only copies 2 KB every 5 ms, and the DMA ring holds
 * 40 ms. At 6 it waited up to 37 ms behind a viewer's video and overflowed. */
#define TASK_PRIO 19
/* The bridge sends 32-bit slots; the top 16 bits are the sample. */
#define SLOT_BITS 32

struct kvm_audio_reader {
    QueueHandle_t queue;
    StaticQueue_t queue_state;
    uint8_t *storage;
    uint32_t dropped;
};

/* What the DMA interrupt saw: when a block landed and where it starts. */
typedef struct {
    int64_t end_us;
    uint64_t frame_index;
    uint32_t epoch;
} stamp_t;

/* Large, so in PSRAM, owned by the task. */
typedef struct {
    int32_t raw[KVM_AUDIO_MAX_FRAMES * KVM_AUDIO_CHANNELS];
    kvm_audio_block_t block;
    kvm_audio_block_t old;
} work_t;

static SemaphoreHandle_t s_mu; /* readers, settings */
static portMUX_TYPE s_isr_mu = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_stamps;
static TaskHandle_t s_task;
static work_t *s_work;
static kvm_audio_reader_t *s_readers[MAX_READERS];

static bool s_enabled;
/* aud_usb as it was at boot: kvm_hid built the USB device from that value. */
static bool s_usb;
static uint64_t s_usb_index;
static int s_bclk = -1, s_ws = -1, s_din = -1;
static volatile bool s_reopen;

/* Task-only, apart from the ISR fields under s_isr_mu. */
static i2s_chan_handle_t s_rx;
static uint32_t s_rate;
static uint32_t s_block_frames;
static uint64_t s_dma_index; /* ISR */
static uint32_t s_epoch;     /* ISR */
static uint32_t s_overruns;  /* ISR */
static uint32_t s_overruns_seen;

static kvm_audio_status_t s_st; /* under s_isr_mu */
static int64_t s_last_isr_us;   /* under s_isr_mu */
static int64_t s_last_read_us;

static bool IRAM_ATTR on_recv(i2s_chan_handle_t ch, i2s_event_data_t *ev, void *ctx)
{
    (void)ch;
    (void)ev;
    (void)ctx;
    stamp_t st = {.end_us = esp_timer_get_time()};
    portENTER_CRITICAL_ISR(&s_isr_mu);
    if (s_last_isr_us && st.end_us - s_last_isr_us > s_st.isr_gap_max_us) {
        s_st.isr_gap_max_us = (uint32_t)(st.end_us - s_last_isr_us);
    }
    s_last_isr_us = st.end_us;
    st.frame_index = s_dma_index;
    st.epoch = s_epoch;
    s_dma_index += s_block_frames;
    portEXIT_CRITICAL_ISR(&s_isr_mu);
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(s_stamps, &st, &wake) != pdTRUE) {
        portENTER_CRITICAL_ISR(&s_isr_mu);
        s_overruns++;
        portEXIT_CRITICAL_ISR(&s_isr_mu);
    }
    return wake == pdTRUE;
}

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t ch, i2s_event_data_t *ev, void *ctx)
{
    (void)ch;
    (void)ev;
    (void)ctx;
    portENTER_CRITICAL_ISR(&s_isr_mu);
    s_overruns++;
    portEXIT_CRITICAL_ISR(&s_isr_mu);
    return false;
}

static bool rate_supported(uint32_t hz)
{
    return hz == 32000 || hz == 44100 || hz == 48000 || hz == 88200 || hz == 96000;
}

static void rx_close(void)
{
    if (!s_rx) {
        return;
    }
    (void)i2s_channel_disable(s_rx);
    (void)i2s_del_channel(s_rx);
    s_rx = NULL;
    s_rate = 0;
    portENTER_CRITICAL(&s_isr_mu);
    s_st.running = false;
    s_st.rate = 0;
    portEXIT_CRITICAL(&s_isr_mu);
    ESP_LOGI(TAG, "receiver closed");
}

/* With the channel stopped, so the stamps and the driver's own queue are both
 * empty and start counting together. */
static esp_err_t rx_restart(void)
{
    xQueueReset(s_stamps);
    portENTER_CRITICAL(&s_isr_mu);
    s_epoch++;
    s_dma_index = 0;
    s_overruns_seen = s_overruns;
    s_st.restarts++;
    s_last_isr_us = 0;
    portEXIT_CRITICAL(&s_isr_mu);
    s_last_read_us = 0;
    return i2s_channel_enable(s_rx);
}

static esp_err_t rx_open(uint32_t rate, int bclk, int ws, int din)
{
    s_block_frames = rate / 200u;
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_SLAVE);
    ch.dma_desc_num = DMA_BUFFERS;
    ch.dma_frame_num = s_block_frames; /* one interrupt, one stamp per block */
    esp_err_t err = i2s_new_channel(&ch, NULL, &s_rx);
    if (err != ESP_OK) {
        s_rx = NULL;
        return err;
    }
    /* In slave mode the rate only sizes the internal clock that samples BCLK. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(SLOT_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = bclk, .ws = ws, .dout = I2S_GPIO_UNUSED, .din = din},
    };
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    /* Before rev 3 there is no 160 MHz PLL for I2S and the default is the
     * 40 MHz crystal: at 48 kHz the slave wants an internal clock of 24.6 MHz
     * and a source twice that. Nothing else here uses the APLL. */
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif
    err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err == ESP_OK) {
        const i2s_event_callbacks_t cb = {.on_recv = on_recv, .on_recv_q_ovf = on_overflow};
        err = i2s_channel_register_event_callback(s_rx, &cb, NULL);
    }
    if (err == ESP_OK) {
        err = rx_restart();
    }
    if (err != ESP_OK) {
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    s_rate = rate;
    portENTER_CRITICAL(&s_isr_mu);
    s_st.running = true;
    s_st.rate = rate;
    portEXIT_CRITICAL(&s_isr_mu);
    ESP_LOGI(TAG, "receiver open: %lu Hz, BCLK %d, WS %d, DATA %d", (unsigned long)rate, bclk, ws, din);
    return ESP_OK;
}

/* Hand one block to every reader; a full queue loses its oldest block. */
static void deliver(const kvm_audio_block_t *b)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; i++) {
        kvm_audio_reader_t *r = s_readers[i];
        if (!r) {
            continue;
        }
        if (xQueueSend(r->queue, b, 0) != pdTRUE) {
            if (xQueueReceive(r->queue, &s_work->old, 0) == pdTRUE) {
                r->dropped++;
            }
            (void)xQueueSend(r->queue, b, 0);
        }
    }
    xSemaphoreGive(s_mu);
}

/* The status meter: how loud the block was and when it ended. */
static void note_levels(const kvm_audio_block_t *b, int64_t end_us)
{
    const unsigned n = (unsigned)b->frames * KVM_AUDIO_CHANNELS;
    int32_t peak = 0;
    uint64_t energy = 0;
    for (unsigned i = 0; i < n; i++) {
        const int32_t v = b->pcm[i];
        const int32_t m = abs(v);
        if (m > peak) {
            peak = m;
        }
        energy += (uint64_t)(v * v);
    }
    portENTER_CRITICAL(&s_isr_mu);
    s_st.frames += b->frames;
    s_st.peak = (float)peak / 32768.0f;
    s_st.rms = n ? sqrtf((float)energy / (float)n) / 32768.0f : 0.0f;
    s_st.last_us = end_us;
    portEXIT_CRITICAL(&s_isr_mu);
}

/* Read one block and pass it on. False when the receiver had to restart. */
static bool take_block(void)
{
    const size_t want = (size_t)s_block_frames * KVM_AUDIO_CHANNELS * (SLOT_BITS / 8);
    size_t got = 0;
    const esp_err_t err = i2s_channel_read(s_rx, s_work->raw, want, &got, 100);
    const int64_t now = esp_timer_get_time();
    if (got && s_last_read_us && now - s_last_read_us > s_st.read_gap_max_us) {
        s_st.read_gap_max_us = (uint32_t)(now - s_last_read_us);
    }
    if (got) {
        s_last_read_us = now;
    }
    if (err == ESP_ERR_TIMEOUT && got == 0) {
        return true; /* no clocks: the source is silent or gone */
    }
    stamp_t st;
    const bool stamped = xQueueReceive(s_stamps, &st, 0) == pdTRUE;
    portENTER_CRITICAL(&s_isr_mu);
    const bool overrun = s_overruns != s_overruns_seen;
    portEXIT_CRITICAL(&s_isr_mu);
    if (err != ESP_OK || got != want || !stamped || overrun) {
        /* The data and its stamp no longer pair up; start both over. */
        (void)i2s_channel_disable(s_rx);
        if (rx_restart() != ESP_OK) {
            rx_close();
        }
        return false;
    }

    kvm_audio_block_t *b = &s_work->block;
    b->frames = (uint16_t)s_block_frames;
    b->rate = s_rate;
    b->frame_index = st.frame_index;
    b->epoch = st.epoch;
    b->start_us = st.end_us - (int64_t)s_block_frames * 1000000 / s_rate;
    const unsigned n = s_block_frames * KVM_AUDIO_CHANNELS;
    for (unsigned i = 0; i < n; i++) {
        b->pcm[i] = (int16_t)(s_work->raw[i] >> 16);
    }
    note_levels(b, st.end_us);
    deliver(b);
    return true;
}

/*
 * The USB speaker: wait for the ISR's wake-up, pass on every whole block, and
 * keep nothing while nobody reads or the target is not playing.
 */
static void usb_step(bool on)
{
    const bool playing = on && usb_src_streaming();
    portENTER_CRITICAL(&s_isr_mu);
    s_st.running = playing;
    s_st.rate = playing ? usb_src_rate() : 0;
    portEXIT_CRITICAL(&s_isr_mu);
    if (!playing) {
        usb_src_drain();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        return;
    }
    /* Woken every 5 ms of sound; the timeout only covers a lost wake-up. */
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    kvm_audio_block_t *b = &s_work->block;
    const uint16_t frames = (uint16_t)(usb_src_rate() / 200u);
    for (;;) {
        int64_t start_us = 0;
        const usb_take_t t = usb_src_take(b->pcm, frames, &start_us);
        if (t == USB_TAKE_NONE) {
            return;
        }
        if (t == USB_TAKE_GAP) {
            portENTER_CRITICAL(&s_isr_mu);
            s_epoch++;
            s_overruns++;
            s_overruns_seen = s_overruns;
            s_st.restarts++;
            portEXIT_CRITICAL(&s_isr_mu);
            s_usb_index = 0;
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (s_last_read_us && now - s_last_read_us > s_st.read_gap_max_us) {
            s_st.read_gap_max_us = (uint32_t)(now - s_last_read_us);
        }
        s_last_read_us = now;
        b->frames = frames;
        b->rate = usb_src_rate();
        b->frame_index = s_usb_index;
        portENTER_CRITICAL(&s_isr_mu);
        b->epoch = s_epoch;
        portEXIT_CRITICAL(&s_isr_mu);
        b->start_us = start_us;
        s_usb_index += frames;
        note_levels(b, start_us + (int64_t)frames * 1000000 / b->rate);
        deliver(b);
    }
}

static bool wanted(int *bclk, int *ws, int *din)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool any = false;
    for (int i = 0; i < MAX_READERS; i++) {
        any |= s_readers[i] != NULL;
    }
    *bclk = s_bclk;
    *ws = s_ws;
    *din = s_din;
    const bool on = s_enabled && any;
    xSemaphoreGive(s_mu);
    return on;
}

static void audio_task(void *arg)
{
    (void)arg;
    bool bridge_seen = false;
    for (;;) {
        /* Capture finds the bridge in its own time; whether it has sound
         * changes what the capability says. */
        if (!bridge_seen && capture_bridge()) {
            bridge_seen = true;
            kvm_audio_apply();
        }
        int bclk, ws, din;
        const bool on = wanted(&bclk, &ws, &din);
        if (s_usb) {
            rx_close();
            usb_step(on);
            continue;
        }
        kvm_video_status_t vs;
        capture_status_get(&vs);
        const uint32_t rate = on && rate_supported(vs.audio_hz) ? vs.audio_hz : 0;
        if (rate == 0 || s_reopen || rate != s_rate) {
            rx_close();
            s_reopen = false;
        }
        if (rate == 0) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (!s_rx && rx_open(rate, bclk, ws, din) != ESP_OK) {
            ESP_LOGW(TAG, "receiver did not open at %lu Hz", (unsigned long)rate);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        (void)take_block();
    }
}

esp_err_t kvm_audio_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_mu = xSemaphoreCreateMutex();
    s_stamps = xQueueCreate(STAMPS, sizeof(stamp_t));
    s_work = heap_caps_calloc(1, sizeof(*s_work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mu || !s_stamps || !s_work || opus_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    s_usb = kvm_setting_bool("aud_usb");
    /* Stack in PSRAM: the task never runs with the flash cache off. */
    if (xTaskCreateWithCaps(audio_task, "audio", TASK_STACK, NULL, TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_usb) {
        usb_src_init(s_task, (uint16_t)(usb_src_rate() / 200u));
    }
    return ESP_OK;
}

void kvm_audio_apply(void)
{
    if (!s_task) {
        return;
    }
    const bool en = kvm_setting_bool("aud_enable");
    const int bclk = (int)kvm_setting_int("aud_bclk");
    const int ws = (int)kvm_setting_int("aud_ws");
    const int din = (int)kvm_setting_int("aud_din");
    const bool pins = bclk >= 0 && ws >= 0 && din >= 0 && bclk != ws && bclk != din && ws != din;
    /* The LT6911D modules bring no I2S out; unknown until capture finds the chip. */
    const kvm_bridge_t *b = capture_bridge();
    const bool silent = b && !b->i2s_audio;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_enabled = s_usb || (en && pins && !silent);
    s_bclk = bclk;
    s_ws = ws;
    s_din = din;
    xSemaphoreGive(s_mu);
    s_reopen = true;
    xTaskNotifyGive(s_task);

    /* Like ATX: available means wired; switched off is the setting's own. */
    if (s_usb) {
        kvm_cap_report(KVM_CAP_AUDIO, true, NULL);
    } else if (silent) {
        kvm_cap_report(KVM_CAP_AUDIO, false,
                       "the %s on this board sends no sound out; turn on Sound over USB", b->name);
    } else if (pins) {
        kvm_cap_report(KVM_CAP_AUDIO, true, NULL);
    } else if (!kvm_cap_available(KVM_CAP_GPIO)) {
        kvm_cap_report(KVM_CAP_AUDIO, false, "turn on Sound over USB in Settings");
    } else {
        kvm_cap_report(KVM_CAP_AUDIO, false,
                       "pick the BCLK, WS and DATA pins in Settings, or turn on Sound over USB");
    }
}

esp_err_t kvm_audio_open(uint16_t depth, kvm_audio_reader_t **out)
{
    *out = NULL;
    if (!s_task || depth == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_audio_reader_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    r->storage = heap_caps_malloc((size_t)depth * sizeof(kvm_audio_block_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->storage) {
        free(r);
        return ESP_ERR_NO_MEM;
    }
    r->queue = xQueueCreateStatic(depth, sizeof(kvm_audio_block_t), r->storage, &r->queue_state);

    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_enabled) {
        for (int i = 0; i < MAX_READERS; i++) {
            if (!s_readers[i]) {
                s_readers[i] = r;
                err = ESP_OK;
                break;
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
    xTaskNotifyGive(s_task);
    *out = r;
    return ESP_OK;
}

esp_err_t kvm_audio_read(kvm_audio_reader_t *r, kvm_audio_block_t *block, uint32_t timeout_ms)
{
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && t == 0) {
        t = 1;
    }
    return xQueueReceive(r->queue, block, t) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint32_t kvm_audio_dropped(const kvm_audio_reader_t *r)
{
    return r->dropped;
}

void kvm_audio_close(kvm_audio_reader_t *r)
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

void kvm_audio_status(kvm_audio_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_task) {
        return;
    }
    portENTER_CRITICAL(&s_isr_mu);
    *out = s_st;
    out->overruns = s_overruns;
    portEXIT_CRITICAL(&s_isr_mu);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    out->enabled = s_enabled;
    out->bclk = s_bclk;
    out->ws = s_ws;
    out->din = s_din;
    for (int i = 0; i < MAX_READERS; i++) {
        out->readers += s_readers[i] != NULL;
    }
    xSemaphoreGive(s_mu);
}
