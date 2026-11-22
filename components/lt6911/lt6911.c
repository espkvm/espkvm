/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lontium LT6911D - the bridge on M5Stack's Add-on Display In.
 *
 * This is not a driver yet, it is a way to find out whether one is needed. The
 * chip runs its own firmware: it holds the EDID, locks to the source and decides
 * its own MIPI output without being told - a PC plugged into the add-on sees a
 * monitor before this firmware has said a word to it. So the open question is
 * whether the CSI receiver can simply be switched on and given frames.
 *
 * What is here answers that. Detect finds the chip, reads its ID out of the
 * bank-switched register space and says so in the log; the mode is declared from
 * Kconfig rather than measured, because reading it back needs register maps this
 * project does not have. Everything else a bridge can be asked to do is left out
 * - the helpers in kvm_bridge.h treat a missing operation as "nothing to do".
 *
 * The answer, on hardware, is no. The CSI receiver was set up for the mode the
 * source was sending - 720p60 and 1080p30, in RGB888 and in YUV422, at 972 and at
 * 714 Mbit/s a lane - and the MIPI output was switched on through 0xb0, which read
 * back 0x00 before and 0x01 after, so the write reaches the chip. Not one DMA
 * completion arrived in any of it.
 *
 * What programs the output is the chip's own firmware, not the host: Espressif's
 * mode "register tables" hold a single bank switch and nothing else, and their
 * driver ships an image (LT6911D_v1_default.hex) with instructions to burn it
 * first. The one in this add-on is M5Stack's, and it does not raise the lanes.
 *
 * So this file is a finding, not a driver, and it is off unless
 * CONFIG_KVM_LT6911 says otherwise - left in because the next attempt
 * starts here, and the next attempt is about firmware, not registers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kvm_bridge.h"

#if CONFIG_KVM_LT6911

static const char *TAG = "lt6911";

/* 0x2b is 0x56 - the address M5Stack publishes - as a 7-bit one. */
#define LT6911_I2C_ADDR 0x2b

/* Bank-switched register space: 0xff picks the bank, and 0xee in bank 0xe0
 * enables register access at all. The chip ID is the first two of bank 0xe1.
 * Sequence taken from Espressif's esp_cam_sensor LT6911 driver. */
#define LT6911_REG_BANK 0xff
/* 0xee opens the chip's internal register bus to I2C. Lontium's own Linux
 * drivers open it for one access and close it again; leaving it open holds the
 * bridge's firmware off its own registers, and that firmware is what locks to
 * HDMI and raises the lanes. So every access here is a pair. */
#define LT6911_REG_SYS_CTLR 0xee
#define LT6911_BANK_SYS 0xe0
#define LT6911_BANK_ID 0xe1
#define LT6911_REG_CHIP_ID_H 0x00
#define LT6911_REG_CHIP_ID_L 0x01
/* MIPI TX: in bank 0xe0, 0x01 puts the output on, 0x00 takes it off. Espressif's
 * driver reads it first and only writes when the state differs. */
#define LT6911_REG_STREAM_CTLR 0xb0

/*
 * The mode the chip has measured, in bank 0xe0. Found by reading every bank with
 * a known signal on the wire and looking for the numbers: nothing documents
 * these, and Espressif's driver does not read them at all. The horizontal pair
 * counts two pixels at a time, which is how the part moves them.
 *
 *   0x80..0x83  NOT a pixel clock, whatever the Linux drivers call it: on this
 *               add-on it reads 25 05 13 01 always - with a mode, with none,
 *               with the source asleep. It looks like a firmware date.
 *   0x88..0x89  htotal / 2, big-endian
 *   0x8a..0x8b  vtotal
 *   0x8c..0x8d  active pixels / 2
 *   0x8e..0x8f  active lines
 *
 * They stay zero for about 1.5 s after the chip locks, and go back to zero when
 * the source goes away - which is what tells this driver there is no signal.
 */
#define LT6911_REG_PIXCLK_MHZ 0x80
#define LT6911_REG_TIMINGS 0x88

typedef struct {
    i2c_master_dev_handle_t i2c;
    bool had_mode; /* so a signal coming and going is said once, not per poll */
    bool tell_empty; /* no mode since start-up or a reset: say what the chip reads, once */
    int64_t read_us;            /* when the timings below were read */
    int64_t quiet_until_us;     /* leave the register bus alone until then */
    int64_t tx_kick_us;         /* when the transmitter was last checked */
    kvm_bridge_timings_t last;  /* what that read returned */
} lt6911_t;

static esp_err_t reg_write(lt6911_t *d, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(d->i2c, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(lt6911_t *d, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(d->i2c, &reg, 1, val, 1, 100);
}

/* Open the register bus, and hand it back. Bank 0xe0 holds the switch. */
static esp_err_t access_open(lt6911_t *d)
{
    ESP_RETURN_ON_ERROR(reg_write(d, LT6911_REG_BANK, LT6911_BANK_SYS), TAG, "bank");
    return reg_write(d, LT6911_REG_SYS_CTLR, 0x01);
}

static void access_close(lt6911_t *d)
{
    (void)reg_write(d, LT6911_REG_BANK, LT6911_BANK_SYS);
    (void)reg_write(d, LT6911_REG_SYS_CTLR, 0x00);
}

/* The ID is logged, not judged: this firmware has never seen one of these, so a
 * number it does not recognise is a thing to write down rather than a reason to
 * walk away from a chip that plainly answers. */
static void log_chip_id(lt6911_t *d)
{
    uint8_t hi = 0, lo = 0;
    esp_err_t err = access_open(d);
    if (err == ESP_OK) {
        err = reg_write(d, LT6911_REG_BANK, LT6911_BANK_ID);
    }
    if (err == ESP_OK) {
        err = reg_read(d, LT6911_REG_CHIP_ID_H, &hi);
    }
    if (err == ESP_OK) {
        err = reg_read(d, LT6911_REG_CHIP_ID_L, &lo);
    }
    access_close(d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chip answers at 0x%02x but the ID read failed: %s", LT6911_I2C_ADDR,
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "chip id 0x%02x%02x", hi, lo);
}

/*
 * Turn the MIPI output on, the way Espressif's driver does.
 *
 * Nothing else here writes to the chip, and this is the one write worth trying
 * blind: the bridge locks to HDMI by itself, so if its output is simply parked
 * this is the switch, and if it is not, the register reads back what it was.
 */
static esp_err_t init_streaming(void *dev)
{
    lt6911_t *d = dev;
    uint8_t before = 0, after = 0;
    ESP_RETURN_ON_ERROR(access_open(d), TAG, "open");
    esp_err_t err = reg_read(d, LT6911_REG_STREAM_CTLR, &before);
    if (err == ESP_OK) {
        err = reg_write(d, LT6911_REG_STREAM_CTLR, 0x01);
    }
    if (err == ESP_OK) {
        err = reg_read(d, LT6911_REG_STREAM_CTLR, &after);
    }
    access_close(d);
    ESP_RETURN_ON_ERROR(err, TAG, "b0");
    ESP_LOGI(TAG, "MIPI TX 0x%02x -> 0x%02x", before, after);
    /*
     * Give the bridge's own firmware time to take the bus back and re-lock.
     *
     * The delay is not enough on its own. Locking takes this chip about two and
     * a half seconds, and every mode read takes its register bus away for the
     * length of thirteen I2C transactions - so a poll landing in the middle of
     * an attempt stops it, and the next poll stops the next one. At start-up
     * that never showed, because the monitor task is not running yet. Resetting
     * the chip to recover it is the same situation without the quiet: three
     * resets in a row each failed to bring a live 1080p15 source back
     * (hardware, 2026-09-20). So say when the bus is the chip's own.
     */
    vTaskDelay(pdMS_TO_TICKS(500));
    d->quiet_until_us = esp_timer_get_time() + 2500000;
    d->tell_empty = true;
    return ESP_OK;
}

static esp_err_t get_timings(void *dev, kvm_bridge_timings_t *out)
{
    (void)dev;
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    lt6911_t *d = dev;

    /*
     * Reading these registers means taking the chip's own register bus away
     * from it for the length of nine I2C transactions, and the monitor asks
     * five times a second. That is fine while a mode is locked and nothing is
     * changing, and it is exactly the wrong thing to do while the chip is
     * trying to re-acquire a source: hold the bus off its firmware then, and it
     * never finishes. A display that blanks and comes back leaves the capture
     * saying "no signal" for ever, which is how this was found - after five
     * hours of uptime and a monitor that went to sleep.
     *
     * So: re-read twice a second with a mode in hand, and once every two
     * seconds without one, which is when the chip needs its bus most.
     */
    const int64_t now_us = esp_timer_get_time();
    const int64_t period_us = d->had_mode ? 500000 : 2000000;
    if ((d->read_us && now_us - d->read_us < period_us) || now_us < d->quiet_until_us) {
        *out = d->last;
        return ESP_OK;
    }
    d->read_us = now_us;

    memset(out, 0, sizeof(*out));
    uint8_t r[8] = {0};
    uint8_t clk = 0;
    ESP_RETURN_ON_ERROR(access_open(d), TAG, "open");
    esp_err_t err = reg_read(d, LT6911_REG_PIXCLK_MHZ, &clk);
    for (unsigned i = 0; err == ESP_OK && i < sizeof(r); i++) {
        err = reg_read(d, (uint8_t)(LT6911_REG_TIMINGS + i), &r[i]);
    }
    access_close(d);
    ESP_RETURN_ON_ERROR(err, TAG, "timings");

    const uint16_t htotal = (uint16_t)(((r[0] << 8) | r[1]) * 2u);
    const uint16_t vtotal = (uint16_t)((r[2] << 8) | r[3]);
    const uint16_t hact = (uint16_t)(((r[4] << 8) | r[5]) * 2u);
    const uint16_t vact = (uint16_t)((r[6] << 8) | r[7]);

    /*
     * No ddc5v is reported here, and 0x80 is not a stand-in for it: it reads
     * 25 05 13 01 whatever the source does (see above). Reading it as "a
     * source is on the wire" had the firmware reset the bridge at an empty
     * room. Which register does tell the two apart is not known yet - the dump
     * below is there to find out.
     */

    /*
     * Nothing locked yet, or the source went away. Either way there is no mode,
     * and saying so beats handing the capture a guess it would tear on.
     *
     * Say it once per loss, with what the chip actually returned. A screen that
     * reads "no signal" hours later is otherwise impossible to tell apart from
     * a bridge that has stopped answering - and the ring log will have rolled
     * over by the time anyone looks.
     */
    if (!hact || !vact || hact >= htotal || vact >= vtotal) {
        /*
         * The chip turns its own MIPI transmitter off when the source leaves,
         * and nothing turns it back on: this firmware writes that register once,
         * at start-up, and the capture's recovery path only runs when frames
         * stop arriving - not when there is honestly no signal. So a screen
         * that blanks leaves the board dark for good. Found after five hours of
         * uptime and a monitor that went to sleep; the boot log after it said
         * "MIPI TX 0x00 -> 0x01", which is the transmitter having been off all
         * along. Put it back, slowly, while there is nothing else to do.
         */
        if (now_us - d->tx_kick_us > 5000000) {
            d->tx_kick_us = now_us;
            uint8_t tx = 0;
            if (access_open(d) == ESP_OK) {
                if (reg_read(d, LT6911_REG_STREAM_CTLR, &tx) == ESP_OK && tx != 0x01) {
                    (void)reg_write(d, LT6911_REG_STREAM_CTLR, 0x01);
                    ESP_LOGI(TAG, "MIPI TX was 0x%02x with no signal; switched back on", tx);
                }
                access_close(d);
            }
        }
        /* Also once after start-up and after each reset, when there has never
           been a mode: a clock of 0 there means nothing on the wire at all,
           anything else a source the chip cannot lock to. */
        if (d->had_mode || d->tell_empty) {
            ESP_LOGW(TAG, "%s: 0x80 %02x, timings %02x %02x %02x %02x %02x %02x %02x %02x",
                     d->had_mode ? "no mode from the chip" : "no mode yet",
                     (unsigned)clk, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            d->had_mode = false;
            d->tell_empty = false;
            /*
             * A picture of bank 0xe0 at the moment the mode went, once per
             * loss. A screen going to sleep and a chip that has wedged look the
             * same from the outside, and telling them apart needs a register
             * that differs between the two - which nothing documents. Two of
             * these dumps, one of each, is what finding it takes.
             */
            uint8_t d0[32] = {0};
            if (access_open(d) == ESP_OK) {
                for (unsigned i = 0; i < sizeof(d0); i++) {
                    if (reg_read(d, (uint8_t)(0x80 + i), &d0[i]) != ESP_OK) {
                        break;
                    }
                }
                access_close(d);
                char hex[sizeof(d0) * 3 + 1];
                for (unsigned i = 0; i < sizeof(d0); i++) {
                    snprintf(hex + i * 3, 4, "%02x ", d0[i]);
                }
                ESP_LOGW(TAG, "bank e0 0x80..0x9f: %s", hex);
            }
            uint8_t hi = 0, lo = 0;
            if (access_open(d) == ESP_OK) {
                if (reg_write(d, LT6911_REG_BANK, LT6911_BANK_ID) == ESP_OK) {
                    (void)reg_read(d, LT6911_REG_CHIP_ID_H, &hi);
                    (void)reg_read(d, LT6911_REG_CHIP_ID_L, &lo);
                }
                access_close(d);
            }
            ESP_LOGW(TAG, "chip still answers with id 0x%02x%02x", hi, lo);
        }
        d->last = *out;
        return ESP_OK;
    }
    if (!d->had_mode) {
        d->had_mode = true;
        ESP_LOGI(TAG, "mode back: %ux%u", (unsigned)hact, (unsigned)vact);
    }

    out->hact = hact;
    out->vact = vact;
    out->htotal = htotal;
    out->vtotal = vtotal;
    /* The refresh rate is not known: there is no pixel clock to work it out
     * from (see 0x80 above). Worked out from that byte it read 15 Hz for a
     * 1080p60 source. 0 is the honest answer, and the capture reads it as
     * "rate unknown". */
    out->hz = 0;
    out->ddc5v = true;
    out->tmds = true;
    out->hdmi_mode = true;
    out->sync = true;
    d->last = *out;
    return ESP_OK;
}

static void remove_dev(void *dev)
{
    lt6911_t *d = dev;
    if (!d) {
        return;
    }
    if (d->i2c) {
        (void)i2c_master_bus_rm_device(d->i2c);
    }
    free(d);
}

/*
 * What "recover" can mean on this bridge. It owns its own hotplug line and
 * nothing here can pull it, so there is no hotplug_reset to give - but the MIPI
 * transmitter is a register, and a source that came and went may have left it
 * off. Turning it back on is the one thing worth doing before the receiver is
 * rebuilt, and it costs one pass over the register bus rather than the constant
 * polling that reading the mode does.
 */
static esp_err_t reapply_csi_path(void *dev)
{
    return init_streaming(dev);
}

static const kvm_bridge_ops_t s_ops = {
    .init_streaming = init_streaming,
    .reapply_csi_path = reapply_csi_path,
    .get_timings = get_timings,
    .remove = remove_dev,
};

static esp_err_t lt6911_detect(i2c_master_bus_handle_t bus, kvm_bridge_t *out)
{
    if (i2c_master_probe(bus, LT6911_I2C_ADDR, 100) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    lt6911_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LT6911_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &d->i2c);
    if (err != ESP_OK) {
        free(d);
        ESP_LOGE(TAG, "i2c add device fail %s", esp_err_to_name(err));
        return err;
    }

    log_chip_id(d);

    out->name = "LT6911D";
    out->yuv422_only = true;
    out->ops = &s_ops;
    out->dev = d;
    return ESP_OK;
}

KVM_BRIDGE_DRIVER(lt6911, lt6911_detect)

#endif /* CONFIG_KVM_LT6911 */
