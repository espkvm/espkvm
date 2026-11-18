/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A knock on the board's speaker. Everything is opened for the sound and
 * closed after it: the I2S channel, the codec and the amplifier. A knock is
 * half a second a few times a day, and an amplifier left on hisses.
 *
 * The codec is probed on the capture chip's I2C bus before any pin is driven,
 * so a board built with the speaker but without the codec fitted never sees
 * its pins touched. A pin the operator gave to something else (the HDMI audio
 * wires on a header, say) stops the knock too.
 */
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_KVM_ENABLE_SPEAKER
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "capture.h"
#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_speaker.h"

static const char *TAG = "speaker";

#define RATE 16000
#define CODEC_ADDR_7BIT 0x18
#define KNOCK_EVERY_US (30LL * 1000 * 1000)

static atomic_bool s_busy;
static int64_t s_last_knock_us;
static bool s_present;

static const int s_pins[] = {
    CONFIG_KVM_SPK_MCLK_GPIO, CONFIG_KVM_SPK_BCLK_GPIO, CONFIG_KVM_SPK_WS_GPIO,
    CONFIG_KVM_SPK_DOUT_GPIO, CONFIG_KVM_SPK_PA_GPIO,
};

/*
 * One knock: a low thump that dies in a few tens of milliseconds, two partials
 * and a click of noise at the start, the way a knuckle on a wooden door sounds
 * small. Two of them, 170 ms apart. Soft at the start so the amplifier does
 * not pop.
 */
#define KNOCK_MS 90
#define GAP_MS 170
#define LEAD_MS 40
#define TOTAL_MS (LEAD_MS + GAP_MS + KNOCK_MS + 60)

static int16_t knock_sample(int n, float level, uint32_t *noise)
{
    const float t_ms = (float)n * 1000.0f / RATE;
    float v = 0;
    for (int k = 0; k < 2; k++) {
        const float t0 = LEAD_MS + k * GAP_MS;
        const float t = (t_ms - t0) / 1000.0f;
        if (t < 0 || t > KNOCK_MS / 1000.0f) {
            continue;
        }
        const float attack = t < 0.002f ? t / 0.002f : 1.0f;
        const float env = attack * expf(-t / 0.018f);
        *noise = *noise * 1664525u + 1013904223u;
        const float white = ((int32_t)(*noise >> 8) - (1 << 23)) / (float)(1 << 23);
        v += env * (0.65f * sinf(2 * (float)M_PI * 170.0f * t) +
                    0.25f * sinf(2 * (float)M_PI * 310.0f * t) +
                    0.10f * white * expf(-t / 0.004f));
    }
    return (int16_t)(v * level * 32767.0f);
}

static bool pins_free(void)
{
    for (size_t i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        const char *user = kvm_settings_pin_user(s_pins[i]);
        if (user) {
            ESP_LOGW(TAG, "no knock: GPIO %d is the speaker's, and '%s' uses it", s_pins[i], user);
            return false;
        }
    }
    return true;
}

static void play(void)
{
    i2c_master_bus_handle_t bus = capture_i2c_bus();
    i2s_chan_handle_t tx = NULL;
    const audio_codec_ctrl_if_t *ctrl = NULL;
    const audio_codec_data_if_t *data = NULL;
    const audio_codec_gpio_if_t *gpio = NULL;
    const audio_codec_if_t *codec = NULL;
    esp_codec_dev_handle_t dev = NULL;
    int16_t *buf = NULL;

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ch.auto_clear = true;
    if (!bus || i2s_new_channel(&ch, &tx, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "no I2S channel or no bus for the speaker");
        return;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_KVM_SPK_MCLK_GPIO,
            .bclk = CONFIG_KVM_SPK_BCLK_GPIO,
            .ws = CONFIG_KVM_SPK_WS_GPIO,
            .dout = CONFIG_KVM_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    /* No 160 MHz PLL for I2S before rev 3 (see kvm_audio.c). */
    std.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
        ESP_LOGW(TAG, "I2S did not start");
        goto out;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    audio_codec_i2s_cfg_t i2s_cfg = {.tx_handle = tx};
    ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    data = audio_codec_new_i2s_data(&i2s_cfg);
    gpio = audio_codec_new_gpio();
    if (!ctrl || !data || !gpio) {
        goto out;
    }
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = CONFIG_KVM_SPK_PA_GPIO,
        .pa_reverted = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    codec = es8311_codec_new(&es);
    if (!codec) {
        goto out;
    }
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data};
    dev = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = RATE};
    if (!dev || esp_codec_dev_open(dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "the codec did not open");
        goto out;
    }
    esp_codec_dev_set_out_vol(dev, kvm_setting_int("spk_volume"));

    /* Written in small pieces: the whole knock would be 22 KB at once. */
    enum { CHUNK = 256 };
    buf = heap_caps_malloc(CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        goto out;
    }
    uint32_t noise = 0x1234567u;
    const int total = RATE * TOTAL_MS / 1000;
    for (int n = 0; n < total; n += CHUNK) {
        for (int i = 0; i < CHUNK; i++) {
            const int16_t v = knock_sample(n + i, 0.8f, &noise);
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
        }
        esp_codec_dev_write(dev, buf, CHUNK * 2 * sizeof(int16_t));
    }

out:
    if (dev) {
        esp_codec_dev_close(dev); /* the amplifier goes off here */
        esp_codec_dev_delete(dev);
    }
    if (codec) {
        audio_codec_delete_codec_if(codec);
    }
    if (ctrl) {
        audio_codec_delete_ctrl_if(ctrl);
    }
    if (data) {
        audio_codec_delete_data_if(data);
    }
    if (gpio) {
        audio_codec_delete_gpio_if(gpio);
    }
    if (tx) {
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
    }
    heap_caps_free(buf);
}

static void knock_task(void *arg)
{
    (void)arg;
    if (pins_free()) {
        play();
    }
    atomic_store(&s_busy, false);
    vTaskDeleteWithCaps(NULL);
}

void kvm_speaker_console_joined(void)
{
    if (!s_present || !kvm_setting_bool("spk_knock")) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_knock_us && now - s_last_knock_us < KNOCK_EVERY_US) {
        return;
    }
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_busy, &idle, true)) {
        return;
    }
    s_last_knock_us = now;
    if (xTaskCreateWithCaps(knock_task, "knock", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        atomic_store(&s_busy, false);
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    /* The bus belongs to the capture path, which may bring it up a little later. */
    for (int i = 0; i < 30 && capture_i2c_bus_init() != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    i2c_master_bus_handle_t bus = capture_i2c_bus();
    s_present = bus && i2c_master_probe(bus, CODEC_ADDR_7BIT, 50) == ESP_OK;
    kvm_cap_report(KVM_CAP_SPEAKER, s_present, s_present ? NULL : "no speaker codec on this board");
    ESP_LOGI(TAG, "%s", s_present ? "ES8311 codec found" : "no ES8311 codec");
    vTaskDeleteWithCaps(NULL);
}

void kvm_speaker_init(void)
{
    if (xTaskCreateWithCaps(probe_task, "spk_probe", 3072, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        kvm_cap_report(KVM_CAP_SPEAKER, false, "speaker probe did not start");
    }
}

#else /* !CONFIG_KVM_ENABLE_SPEAKER */
#include "kvm_speaker.h"

void kvm_speaker_init(void) {}
void kvm_speaker_console_joined(void) {}
#endif
