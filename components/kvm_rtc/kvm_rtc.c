/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Battery-backed clock chips. Each keeps the time in BCD registers, says in one
 * bit whether that time can be trusted (an oscillator that stopped, a battery
 * that ran low), and is written once the device knows the time from somewhere
 * else. The chip keeps UTC; the time zone stays a setting.
 *
 * Two addresses are crowded, so Auto only takes what it can recognise:
 *  - 0x68: a DS3231 is told by its thermometer (a room temperature, the low six
 *    bits of the fraction always zero). The DS1307, the PCF8523 and the
 *    MPU-6050 motion sensor answer there too, and are left alone - the PCF8523
 *    has to be named in the settings.
 *  - 0x51: a PCF8563 / BM8563 is told by the bits it always reads as zero. An
 *    EEPROM can sit there too, and writing a time into it would spoil it. The
 *    PCF85063 looks too much like a PCF8563 to tell apart, so it is named too.
 *
 * Only the DS3231 has been run on hardware; the others follow their datasheets.
 */
#include "kvm_rtc.h"

#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_settings.h"

static const char *TAG = "rtc";

/* Before this the system clock is taken as unset (no NTP, no browser yet). */
#define CLOCK_VALID_EPOCH 1735689600LL /* 2025-01-01 */
#define SYNC_PERIOD_MIN 10
#define SYNC_DRIFT_S 2

enum { CHIP_AUTO, CHIP_OFF, CHIP_DS3231, CHIP_PCF8563, CHIP_PCF85063, CHIP_PCF8523 };

static i2c_master_dev_handle_t s_dev;
static const struct rtc_drv *s_drv;
/* The chip's own thermometer, in quarter degrees (DS3231 only). */
static volatile int s_temp_q = INT32_MIN;

/* ---- bus helpers -------------------------------------------------------- */

static bool rd(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100) == ESP_OK;
}

static bool wr(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[12];
    if (len + 1 > sizeof(buf)) {
        return false;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 100) == ESP_OK;
}

static int bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0f);
}

static uint8_t tobcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static bool bcd_ok(uint8_t v)
{
    return (v & 0x0f) <= 9 && (v >> 4) <= 9;
}

/* Unix seconds from a UTC date - timegm by hand, since newlib's mktime would
   apply the time zone. -1 when a field is out of range. */
static long long unix_from(int year, int mon, int day, int hour, int min, int sec)
{
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12 ||
        year < 2000 || year > 2199) {
        return -1;
    }
    static const int days[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long long d = (long long)(year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 +
                  (year - 1601) / 400;
    d += days[mon - 1] + day - 1;
    if (mon > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        d++;
    }
    return d * 86400 + hour * 3600 + min * 60 + sec;
}

/* ---- the drivers --------------------------------------------------------- */

typedef struct rtc_drv {
    const char *name;
    uint8_t addr;
    bool (*recognise)(void); /* NULL: never taken by Auto */
    void (*setup)(void);     /* NULL: nothing to set once */
    long long (*read)(void); /* Unix seconds, -1 if it cannot be trusted */
    bool (*write)(time_t now);
    bool (*temp)(int *quarters); /* NULL: no thermometer */
} rtc_drv_t;

/* DS3231 / DS3231M / DS3232: time at 0x00, status 0x0F (OSF bit 7), temp 0x11. */

static bool ds3231_recognise(void)
{
    uint8_t t[2], sec;
    if (!rd(0x11, t, sizeof(t)) || !rd(0x00, &sec, 1)) {
        return false;
    }
    const int c = (int8_t)t[0];
    return (t[1] & 0x3f) == 0 && c >= -10 && c <= 85 && c != 0 && (sec & 0x80) == 0;
}

static long long ds3231_read(void)
{
    uint8_t st, r[7];
    if (!rd(0x0F, &st, 1) || (st & 0x80) || !rd(0x00, r, sizeof(r))) {
        return -1;
    }
    int hour = (r[2] & 0x40) ? bcd(r[2] & 0x1f) % 12 + ((r[2] & 0x20) ? 12 : 0) : bcd(r[2] & 0x3f);
    return unix_from(2000 + bcd(r[6]) + ((r[5] & 0x80) ? 100 : 0), bcd(r[5] & 0x1f), bcd(r[4] & 0x3f),
                     hour, bcd(r[1] & 0x7f), bcd(r[0] & 0x7f));
}

static bool ds3231_write(time_t now)
{
    struct tm t;
    gmtime_r(&now, &t);
    const uint8_t r[7] = {tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), (uint8_t)(t.tm_wday + 1),
                          tobcd(t.tm_mday), (uint8_t)(tobcd(t.tm_mon + 1) | (t.tm_year >= 200 ? 0x80 : 0)),
                          tobcd(t.tm_year % 100)};
    uint8_t st;
    if (!wr(0x00, r, sizeof(r)) || !rd(0x0F, &st, 1)) {
        return false;
    }
    st &= (uint8_t)~0x80; /* the time is good now */
    return wr(0x0F, &st, 1);
}

static bool ds3231_temp(int *q)
{
    uint8_t t[2];
    if (!rd(0x11, t, sizeof(t))) {
        return false;
    }
    *q = (int8_t)t[0] * 4 + (t[1] >> 6);
    return true;
}

/* PCF8563 / BM8563: control 0x00-0x01, time 0x02-0x08 (VL in bit 7 of the
   seconds), CLKOUT 0x0D, timer control 0x0E. */

static bool pcf8563_recognise(void)
{
    uint8_t r[16];
    if (!rd(0x00, r, sizeof(r))) {
        return false;
    }
    /* The bits a PCF8563 always reads as zero, and a time that parses. Bit 2 of
       the timer control (0x0E) is not one of them on the BM8563 in the M5Stack
       Unit RTC: it read 0x07 there, and the old mask turned the chip away. */
    if ((r[0] & 0x57) || (r[1] & 0xe0) || (r[13] & 0x7c) || (r[14] & 0x78) || (r[6] & 0xf8)) {
        ESP_LOGW(TAG, "0x51 answers but is not taken for a PCF8563: %02x %02x .. %02x %02x %02x .. "
                      "%02x %02x",
                 r[0], r[1], r[2], r[5], r[6], r[13], r[14]);
        return false;
    }
    /* VL set: the chip itself says its time is not to be trusted - a new one,
       or one whose battery ran out - so the time may be anything. The M5Stack
       Unit RTC came like that and was turned away until the time check
       stopped applying to it. */
    if (r[2] & 0x80) {
        return true;
    }
    const int day = bcd(r[5] & 0x3f), mon = bcd(r[7] & 0x1f);
    return bcd_ok(r[2] & 0x7f) && bcd_ok(r[3] & 0x7f) && bcd_ok(r[4] & 0x3f) && bcd_ok(r[8]) &&
           day >= 1 && day <= 31 && mon >= 1 && mon <= 12;
}

static long long pcf8563_read(void)
{
    uint8_t r[7];
    if (!rd(0x02, r, sizeof(r)) || (r[0] & 0x80)) {
        return -1;
    }
    return unix_from(2000 + bcd(r[6]), bcd(r[5] & 0x1f), bcd(r[3] & 0x3f), bcd(r[2] & 0x3f),
                     bcd(r[1] & 0x7f), bcd(r[0] & 0x7f));
}

static bool pcf8563_write(time_t now)
{
    struct tm t;
    gmtime_r(&now, &t);
    const uint8_t ctl[2] = {0x00, 0x00}; /* running, no test mode, no interrupts */
    const uint8_t r[7] = {tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday),
                          (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year % 100)};
    return wr(0x00, ctl, sizeof(ctl)) && wr(0x02, r, sizeof(r)); /* seconds clear VL */
}

/* PCF85063: control 0x00 (STOP bit 5), time 0x04-0x0A (OS in bit 7 of the seconds). */

static long long pcf85063_read(void)
{
    uint8_t r[7];
    if (!rd(0x04, r, sizeof(r)) || (r[0] & 0x80)) {
        return -1;
    }
    return unix_from(2000 + bcd(r[6]), bcd(r[5] & 0x1f), bcd(r[3] & 0x3f), bcd(r[2] & 0x3f),
                     bcd(r[1] & 0x7f), bcd(r[0] & 0x7f));
}

static bool pcf85063_write(time_t now)
{
    struct tm t;
    gmtime_r(&now, &t);
    uint8_t c1;
    if (!rd(0x00, &c1, 1)) {
        return false;
    }
    c1 &= (uint8_t)~0x22; /* running, 24-hour */
    const uint8_t r[7] = {tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday),
                          (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year % 100)};
    return wr(0x00, &c1, 1) && wr(0x04, r, sizeof(r)); /* seconds clear OS */
}

/* PCF8523: control 0x00-0x02, time 0x03-0x09 (OS in bit 7 of the seconds). It
   leaves the factory with the battery switch-over turned off (control 3, bits
   7..5 = 111), so without a word from us the battery does nothing at all. */

static void pcf8523_setup(void)
{
    uint8_t c3;
    if (rd(0x02, &c3, 1) && (c3 & 0xe0) == 0xe0) {
        const uint8_t on = 0x00; /* switch over in standard mode, no battery interrupts */
        if (wr(0x02, &on, 1)) {
            ESP_LOGI(TAG, "PCF8523: battery switch-over turned on");
        }
    }
}

static long long pcf8523_read(void)
{
    uint8_t r[7];
    if (!rd(0x03, r, sizeof(r)) || (r[0] & 0x80)) {
        return -1;
    }
    return unix_from(2000 + bcd(r[6]), bcd(r[5] & 0x1f), bcd(r[3] & 0x3f), bcd(r[2] & 0x3f),
                     bcd(r[1] & 0x7f), bcd(r[0] & 0x7f));
}

static bool pcf8523_write(time_t now)
{
    struct tm t;
    gmtime_r(&now, &t);
    uint8_t c1;
    if (!rd(0x00, &c1, 1)) {
        return false;
    }
    c1 &= (uint8_t)~0x28; /* running (STOP), 24-hour */
    const uint8_t r[7] = {tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday),
                          (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year % 100)};
    return wr(0x00, &c1, 1) && wr(0x03, r, sizeof(r)); /* seconds clear OS */
}

static const rtc_drv_t k_drivers[] = {
    [CHIP_DS3231] = {"DS3231", 0x68, ds3231_recognise, NULL, ds3231_read, ds3231_write, ds3231_temp},
    [CHIP_PCF8563] = {"PCF8563", 0x51, pcf8563_recognise, NULL, pcf8563_read, pcf8563_write, NULL},
    [CHIP_PCF85063] = {"PCF85063", 0x51, NULL, NULL, pcf85063_read, pcf85063_write, NULL},
    [CHIP_PCF8523] = {"PCF8523", 0x68, NULL, pcf8523_setup, pcf8523_read, pcf8523_write, NULL},
};

/* ---- finding it ---------------------------------------------------------- */

static bool attach(i2c_master_bus_handle_t bus, const rtc_drv_t *d, bool must_recognise)
{
    if (i2c_master_probe(bus, d->addr, 50) != ESP_OK) {
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = d->addr,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    if (must_recognise && !(d->recognise && d->recognise())) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    s_drv = d;
    return true;
}

/* The second I2C bus, on @p sda / @p scl: made here, or taken over from the
   status OLED if it made it first on the same pins. */
static i2c_master_bus_handle_t own_bus(int sda, int scl)
{
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(I2C_NUM_1, &bus) == ESP_OK && bus) {
        return bus; /* already made, for the OLED on the same pins */
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = 1},
    };
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "could not make an I2C bus on SDA %d / SCL %d", sda, scl);
        return NULL;
    }
    return bus;
}

/* The bus the operator wired it to: the capture board's, or the second one on
   pins of their own - which the status OLED may already have made. */
static i2c_master_bus_handle_t rtc_bus(i2c_master_bus_handle_t capture_bus)
{
    if (kvm_setting_int("rtc_bus") != 1) {
        return capture_bus;
    }
    const int sda = (int)kvm_setting_int("rtc_sda");
    const int scl = (int)kvm_setting_int("rtc_scl");
    if (sda < 0 || scl < 0) {
        ESP_LOGW(TAG, "clock chip on its own pins, but SDA or SCL is not set");
        return NULL;
    }
    return own_bus(sda, scl);
}

/* "Auto" on the capture bus: try it, and then the status display's own bus if
   the board has one - where a clock module is plugged in beside the display
   (the M5Stack Unit RTC on the Grove port of the Unit PoE-P4), with nothing to
   set. */
static bool auto_attach(i2c_master_bus_handle_t bus)
{
    return attach(bus, &k_drivers[CHIP_DS3231], true) || attach(bus, &k_drivers[CHIP_PCF8563], true);
}

/* ---- keeping it in step -------------------------------------------------- */

static void sync_task(void *arg)
{
    (void)arg;
    int minute = 0;
    for (;;) {
        int q;
        if (s_drv->temp && s_drv->temp(&q)) {
            s_temp_q = q;
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 1000)); /* NTP or a sign-in usually comes within a minute */
        if (minute++ % SYNC_PERIOD_MIN != 0) {
            continue;
        }
        const time_t now = time(NULL);
        if ((long long)now < CLOCK_VALID_EPOCH) {
            continue;
        }
        const long long chip = s_drv->read();
        const long long diff = chip < 0 ? SYNC_DRIFT_S + 1 : chip - (long long)now;
        if (diff > SYNC_DRIFT_S || diff < -SYNC_DRIFT_S) {
            if (s_drv->write(now)) {
                ESP_LOGI(TAG, "%s set from the system clock (was %s)", s_drv->name,
                         chip < 0 ? "not set" : "off by more than 2 s");
            } else {
                ESP_LOGW(TAG, "could not write the %s", s_drv->name);
            }
        }
    }
}

void kvm_rtc_init(i2c_master_bus_handle_t capture_bus)
{
    const int choice = (int)kvm_setting_int("rtc_chip");
    if (s_dev || choice == CHIP_OFF) {
        return;
    }
    i2c_master_bus_handle_t bus = rtc_bus(capture_bus);
    if (!bus) {
        return;
    }
    if (choice == CHIP_AUTO) {
        bool found = auto_attach(bus);
        const int dsda = (int)kvm_setting_int("disp_sda");
        const int dscl = (int)kvm_setting_int("disp_scl");
        /* Only while the display is on: those pins may be a relay or a button
           otherwise, and must not be turned into an I2C bus behind its back. */
        if (!found && kvm_setting_int("rtc_bus") != 1 && kvm_setting_bool("disp_enable") &&
            dsda >= 0 && dscl >= 0) {
            i2c_master_bus_handle_t other = own_bus(dsda, dscl);
            if (other && auto_attach(other)) {
                found = true;
                bus = other;
                ESP_LOGI(TAG, "clock chip found on the display's bus (SDA %d / SCL %d)", dsda, dscl);
            }
        }
        if (!found) {
            if (i2c_master_probe(bus, 0x68, 50) == ESP_OK) {
                ESP_LOGW(TAG, "something answers at 0x68 but it is not a DS3231 (a DS1307, a "
                              "PCF8523, a motion sensor?) - left alone; a PCF8523 can be named "
                              "in Settings");
            }
            return; /* no clock chip: the device goes on as it always has */
        }
    } else if (choice < (int)(sizeof(k_drivers) / sizeof(k_drivers[0])) && k_drivers[choice].name) {
        if (!attach(bus, &k_drivers[choice], false)) {
            ESP_LOGW(TAG, "%s chosen in Settings, but nothing answers at 0x%02x",
                     k_drivers[choice].name, k_drivers[choice].addr);
            return;
        }
    } else {
        return;
    }

    if (s_drv->setup) {
        s_drv->setup();
    }
    const long long chip = s_drv->read();
    if (chip < CLOCK_VALID_EPOCH) {
        ESP_LOGW(TAG, "%s found, but it holds no valid time yet; it is set once the device "
                      "learns the time (NTP, or a browser signing in)",
                 s_drv->name);
    } else if ((long long)time(NULL) < CLOCK_VALID_EPOCH) {
        const struct timeval tv = {.tv_sec = (time_t)chip};
        settimeofday(&tv, NULL);
        const time_t now = (time_t)chip;
        struct tm t;
        gmtime_r(&now, &t);
        char when[24];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &t);
        ESP_LOGI(TAG, "%s found; clock set from it: %s UTC", s_drv->name, when);
    } else {
        ESP_LOGI(TAG, "%s found", s_drv->name);
    }
    xTaskCreate(sync_task, "rtc", 3072, NULL, 2, NULL);
}

bool kvm_rtc_present(void)
{
    return s_dev != NULL;
}

const char *kvm_rtc_name(void)
{
    return s_drv ? s_drv->name : "";
}

bool kvm_rtc_temperature(float *celsius)
{
    const int q = s_temp_q;
    if (!s_dev || q == INT32_MIN) {
        return false;
    }
    *celsius = (float)q / 4.0f;
    return true;
}
