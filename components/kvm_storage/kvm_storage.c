/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "soc/sdmmc_pins.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "kvm_board.h"
#include "kvm_caps.h"
#include "kvm_settings.h"

#define TAG "storage"

#define MOUNT_POINT "/sd"
#define MEDIA_BLOCK_SIZE 512u

/*
 * microSD write needs a bus that carries it. On rev >= 3.0 silicon it does. On
 * pre-3.0 silicon writes timed out at every clock that was tried, and the card was
 * read-only - but that was with the slot's IO LDO off. With it on, a P4-ETH on
 * rev 1.3 writes at 40 MHz (checked with CRC). So a pre-3.0 board writes when it
 * has that LDO set, and stays read-only otherwise.
 */
#define SD_CAN_WRITE \
    (CONFIG_ESP32P4_REV_MIN_300 || CONFIG_KVM_SD_IO_LDO_CHAN >= 0 || CONFIG_KVM_SD_WRITE_NO_LDO)
#define SD_WRITE_UNAVAILABLE_REASON \
    "this board cannot write the microSD reliably; prepare the card in a reader"

static sdmmc_card_t *s_card;

/* ---- bus speed ------------------------------------------------------------
 *
 * The clock steps the bus can run at. The mount starts at the board's fastest
 * (or the operator's choice) and steps down when the card will not mount or a
 * test read fails; after that, any failed transfer steps it down again before
 * the driver retries. Boards and cards differ, so this is found per card, not
 * promised per board.
 *
 * A step down is not for good: while the card is idle, a test read one step up
 * tries to win the speed back. Each failure soon after a climb doubles the wait
 * before the next try, so a card that really cannot go faster costs one failed
 * transfer every few minutes, and a one-off glitch costs half a minute.
 */
static const uint32_t k_sd_steps_khz[] = {40000, 20000, 10000, 4000, 2000};
#define SD_N_STEPS (sizeof(k_sd_steps_khz) / sizeof(k_sd_steps_khz[0]))
#define SD_FLOOR_KHZ 2000u
#define SD_PROBE_FIRST_S 30
#define SD_PROBE_MAX_S 600
#define SD_PROBE_IDLE_US (5 * 1000 * 1000LL)
/* Failures closer together than this are one bad moment, not one rung each. */
#define SD_STEP_BURST_US (500 * 1000LL)
#define SD_PROBE_SECTORS 2048 /* 1 MB: short, a target read may be waiting */
#define SD_MOUNT_TEST_SECTORS 4096

static volatile uint32_t s_bus_khz;
static volatile uint32_t s_bus_errors;
static uint32_t s_start_khz; /* the ceiling a climb stops at */
static volatile int64_t s_last_io_us;
static volatile int64_t s_next_probe_us; /* 0: nothing to win back */
static volatile uint32_t s_probe_wait_s = SD_PROBE_FIRST_S;
static volatile int64_t s_climbed_us;
static volatile bool s_other_slot_busy;
static volatile bool s_probing; /* the probe sets the next wait itself */
static volatile bool s_presence_poll; /* the slot watcher is asking, not reading data */
static volatile bool s_slot_recheck;  /* a real transfer failed; look at the slot now */
static volatile bool s_bus_released;  /* the SD host went to the co-processor */
static void (*s_slot_cb)(void);       /* told when a card appears or goes away */
static void (*s_leaving_cb)(const char *why); /* told before the filesystem goes */

static esp_err_t sd_mount(int attempts);

static uint32_t sd_step_below(uint32_t khz)
{
    for (size_t i = 0; i < SD_N_STEPS; i++) {
        if (k_sd_steps_khz[i] < khz) {
            return k_sd_steps_khz[i];
        }
    }
    return SD_FLOOR_KHZ;
}

static uint32_t sd_step_above(uint32_t khz)
{
    for (size_t i = SD_N_STEPS; i-- > 0;) {
        if (k_sd_steps_khz[i] > khz) {
            return k_sd_steps_khz[i] < s_start_khz ? k_sd_steps_khz[i] : s_start_khz;
        }
    }
    return s_start_khz;
}

/* The fastest clock to try: the "sd_speed" setting, or the board's own limit. */
static uint32_t sd_start_khz(void)
{
    const int32_t choice = kvm_setting_int("sd_speed"); /* 0 auto, then the steps */
    if (choice >= 1 && choice <= (int32_t)SD_N_STEPS) {
        return k_sd_steps_khz[choice - 1];
    }
    return CONFIG_KVM_SD_MAX_KHZ;
}

void kvm_storage_set_other_slot_busy(bool busy)
{
    s_other_slot_busy = busy;
}

void kvm_storage_set_slot_changed_cb(void (*cb)(void))
{
    s_slot_cb = cb;
}

void kvm_storage_set_fs_leaving_cb(void (*cb)(const char *why))
{
    s_leaving_cb = cb;
}

/* Defined with the sources below. */
static const kvm_media_ops_t s_card_file_ops;
static const kvm_media_ops_t s_whole_card_ops;

/* Outside the card lock: whoever listens may take a while to close its files,
 * and a target read must not wait behind that. A medium on the card is taken
 * out first, and the media task waits for the card lock, so no read of it is
 * running when this returns. */
static void fs_leaving(const char *why)
{
    kvm_media_eject_ops(&s_card_file_ops);
    kvm_media_eject_ops(&s_whole_card_ops);
    if (s_leaving_cb) {
        s_leaving_cb(why);
    }
}

void sdmmc_espkvm_transfer_started(sdmmc_card_t *card)
{
    (void)card;
    s_last_io_us = esp_timer_get_time();
}

/* Called by the sdmmc driver after each failed DMA read or write, before it retries. */
void sdmmc_espkvm_transfer_failed(sdmmc_card_t *card, bool write, esp_err_t err)
{
    s_bus_errors++;
    if (s_presence_poll) {
        return; /* only asking whether a card is still there; nothing to slow down */
    }
    /* A card the target is reading never goes idle, so the watcher's five idle
     * seconds never arrive. A read that failed is the other way to find out the
     * card left: ask the watcher to look on its next tick. */
    s_slot_recheck = true;
    const uint32_t lower = sd_step_below(s_bus_khz);
    if (lower >= s_bus_khz) {
        return;
    }
    /* The driver retries a failed transfer at once and the media layer retries
     * after that, so one bad moment arrives as a burst of failures. Step one rung
     * per burst: a card pulled out of the slot otherwise walks the bus from
     * 40 MHz down to the floor in ten milliseconds. */
    static int64_t s_stepped_us;
    const int64_t failed_us = esp_timer_get_time();
    if (s_stepped_us && failed_us - s_stepped_us < SD_STEP_BURST_US) {
        return;
    }
    s_stepped_us = failed_us;
    if (s_probing) {
        ESP_LOGW(TAG, "microSD %s failed at %lu kHz (0x%x); back to %lu kHz",
                 write ? "write" : "read", (unsigned long)s_bus_khz, err, (unsigned long)lower);
    } else {
        const int64_t now = esp_timer_get_time();
        /* Failing soon after a climb means that speed does not hold: wait longer. */
        if (s_climbed_us && now - s_climbed_us < (int64_t)SD_PROBE_MAX_S * 1000000) {
            s_probe_wait_s =
                s_probe_wait_s * 2 < SD_PROBE_MAX_S ? s_probe_wait_s * 2 : SD_PROBE_MAX_S;
        }
        s_climbed_us = 0;
        s_next_probe_us = now + (int64_t)s_probe_wait_s * 1000000;
        ESP_LOGW(TAG, "microSD %s failed at %lu kHz (0x%x); slowing the bus to %lu kHz, next try up in %lu s",
                 write ? "write" : "read", (unsigned long)s_bus_khz, err, (unsigned long)lower,
                 (unsigned long)s_probe_wait_s);
    }
    s_bus_khz = lower;
    (void)sdmmc_host_set_card_clk(card->host.slot, lower);
}

/* Read the first sectors in multi-block reads, the kind the target makes. A
 * failure slows the bus through the hook above, so this reports whether it did. */
#define SD_TEST_CHUNK 256
static bool sd_read_test(size_t sectors)
{
    const size_t len = SD_TEST_CHUNK * s_card->csd.sector_size;
    uint8_t *buf = heap_caps_aligned_alloc(64, len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return true; /* no memory to test with; the runtime fallback still guards */
    }
    const uint32_t errors = s_bus_errors;
    for (size_t sec = 0; sec < sectors && sec + SD_TEST_CHUNK <= s_card->csd.capacity;
         sec += SD_TEST_CHUNK) {
        if (sdmmc_read_sectors(s_card, buf, sec, SD_TEST_CHUNK) != ESP_OK) {
            break;
        }
    }
    free(buf);
    return s_bus_errors == errors;
}

/*
 * Guards s_card against the card leaving while it is read: the media task reads
 * images, the web task uploads, and the slot watcher unmounts a card that was
 * pulled out. Also guards s_rescue_writing. Raw FATFS is used rather than stdio
 * for images: f_lseek takes a 64-bit FSIZE_t that covers exFAT files past
 * 4 GiB, where fseek's long offset would overflow past 2 GiB. Drive "0:" is the
 * same volume esp_vfs_fat mounted. */
static SemaphoreHandle_t s_card_lock;
/* True while the whole card is handed to the target read-write: the firmware then
 * keeps off the filesystem so there is a single writer, and re-reads the card
 * (kvm_storage_reread) when the operator switches the medium back. */
static bool s_handed_over;
/* The rescue partition, found once at init; NULL on a device whose partition
 * table predates it (it is served only when present). */
static const esp_partition_t *s_rescue;
/* An upload in progress: the partition is being erased/written, so it must not
 * be served to the target meanwhile. The flag is read from other tasks (select,
 * begin) under s_card_lock, so every transition of it is made under the lock
 * too; it is volatile so the write path, which reads it without the lock while
 * streaming, always sees the current value. s_rescue_wpos has no cross-task
 * reader - begin/write/end/abort run in sequence on the one upload handler. */
static volatile bool s_rescue_writing;
static size_t s_rescue_wpos;

/* An erased flash sector reads as 0xFF; a written image never leaves its first
 * sector all-ones, so this is how "is there an image at all" is answered
 * without storing a separate flag. */
static bool rescue_has_image(void)
{
    if (!s_rescue) {
        return false;
    }
    uint8_t head[MEDIA_BLOCK_SIZE];
    if (esp_partition_read(s_rescue, 0, head, sizeof(head)) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < sizeof(head); i++) {
        if (head[i] != 0xFF) {
            return true;
        }
    }
    return false;
}

#define SD_PWR_ON_LEVEL (KVM_BOARD_SD_PWR_ACTIVE_LOW ? 0 : 1)

static void slot_power_claim(void)
{
    if (KVM_BOARD_SD_PWR_GPIO < 0) {
        return; /* board has no power gate; the slot is always powered */
    }
    const gpio_config_t cfg = {
        /* The early return above means we never reach here with a negative pin,
         * but the initializer is still compiled, and 1ULL << -1 is a constant
         * negative shift (-Werror). Clamp so the always-powered (-1) build is
         * clean; the value is unused in that case. */
        .pin_bit_mask = 1ULL << (KVM_BOARD_SD_PWR_GPIO < 0 ? 0 : KVM_BOARD_SD_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "could not claim the slot power pin (GPIO %d)", KVM_BOARD_SD_PWR_GPIO);
    }
}

/*
 * Power-cycle the slot: cut power, let the card fully discharge, then restore it
 * and wait for its regulator to settle.
 *
 * A warm reset (esp_restart, or the flasher's RTS pulse) does not power-cycle
 * the card - the gate GPIO simply stays at whatever it was, so the card keeps
 * the state it was left in. If that state was mid-transaction, its next
 * initialisation answers SEND_OP_COND with nothing and the mount times out
 * (0x107), which looks exactly like an empty slot even though the card is
 * seated. Explicitly dropping power first makes every boot start from a cold
 * card, which is the only state initialisation is guaranteed to work from. The
 * discharge needs to be long enough for the rail to actually fall; 3.3 V through
 * the card's bulk capacitance does not vanish instantly.
 */
static void slot_power_settle(void)
{
    /*
     * Assert power and let it settle briefly before the first command. Driving
     * the gate off first to force a cold card was tried and did not help -
     * GPIO45 does not reliably drop the rail - and a longer settle did not
     * improve the intermittent init failures either, so this just asserts power
     * and waits a moment. The real mitigation for the flakiness is the retry
     * loop and staying off the UHS/DDR speed paths.
     */
    if (KVM_BOARD_SD_PWR_GPIO >= 0) {
        gpio_set_level(KVM_BOARD_SD_PWR_GPIO, SD_PWR_ON_LEVEL);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}

const char *kvm_storage_mount_point(void)
{
    return MOUNT_POINT;
}

/* "0:/rel" for raw FatFs. False for a path that could leave the card's root. */
static bool fat_path(char *out, size_t cap, const char *rel)
{
    if (!rel || !rel[0] || rel[0] == '/' || strstr(rel, "..") || strchr(rel, '\\')) {
        return false;
    }
    const int n = snprintf(out, cap, "0:/%s", rel);
    return n > 0 && (size_t)n < cap;
}

esp_err_t kvm_storage_file_size(const char *rel, uint64_t *size)
{
    char path[300];
    if (!size || !fat_path(path, sizeof(path), rel)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fi.fattrib & AM_DIR) {
        return ESP_ERR_INVALID_ARG;
    }
    *size = fi.fsize;
    return ESP_OK;
}

struct kvm_file {
    FIL fil;
};

kvm_file_t *kvm_storage_file_open(const char *rel, uint64_t at)
{
    char path[300];
    if (!fat_path(path, sizeof(path), rel)) {
        return NULL;
    }
    /* Zeroed: f_open allocates the sector buffer only when fil.buf is NULL. */
    kvm_file_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    if (f_open(&f->fil, path, FA_READ) != FR_OK) {
        free(f);
        return NULL;
    }
    if (f_lseek(&f->fil, (FSIZE_t)at) != FR_OK) {
        f_close(&f->fil);
        free(f);
        return NULL;
    }
    return f;
}

int kvm_storage_file_read(kvm_file_t *f, void *buf, size_t len)
{
    UINT br = 0;
    return f && f_read(&f->fil, buf, len, &br) == FR_OK ? (int)br : -1;
}

void kvm_storage_file_close(kvm_file_t *f)
{
    if (f) {
        f_close(&f->fil);
        free(f);
    }
}

bool kvm_storage_writable(void)
{
#if SD_CAN_WRITE
    /* rev >= 3.0 writes the microSD reliably; writable once a card is mounted -
     * unless the whole card is handed to the target, which owns it exclusively. */
    return s_card != NULL && !s_handed_over;
#else
    /* pre-3.0 without the IO LDO: writes time out, the card stays read-only. */
    return false;
#endif
}

const char *kvm_storage_write_unavailable_reason(void)
{
#if SD_CAN_WRITE
    if (s_handed_over) {
        return "the whole card is handed to the target; switch the medium to manage files here";
    }
    /* Otherwise only reached when there is no card in the slot. */
    return s_card ? NULL : "no microSD card in the slot";
#else
    return SD_WRITE_UNAVAILABLE_REASON;
#endif
}

void kvm_storage_status(kvm_storage_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_card) {
        return;
    }
    out->mounted = true;
    snprintf(out->name, sizeof(out->name), "%s", s_card->cid.name);
    int real_khz = 0;
    if (sdmmc_host_get_real_freq(kvm_storage_sd_slot(), &real_khz) == ESP_OK) {
        out->bus_khz = (uint32_t)real_khz;
    }
    out->bus_errors = s_bus_errors;
    out->bus_max_khz = s_start_khz;
    const int64_t next = s_next_probe_us;
    if (next) {
        const int64_t left = next - esp_timer_get_time();
        out->bus_retry_s = left > 0 ? (uint32_t)(left / 1000000) + 1 : 1;
    }

    /* Capacity comes from the card; free space from the filesystem. FATFS
     * reports in clusters, so the two multiply back up to bytes. */
    out->total_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    /* While the target owns the card, keep off the filesystem entirely - a
     * free-space query would read sectors the target may be writing. */
    if (s_handed_over) {
        return;
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs) {
        const uint64_t cluster_bytes = (uint64_t)(fs->csize) * s_card->csd.sector_size;
        out->free_bytes = (uint64_t)free_clusters * cluster_bytes;
    }
}

/* ---- sources for virtual media --------------------------------------------
 *
 * What the target's USB drive can be backed by from here: an image file on the
 * card, the whole card, or the rescue partition. kvm_media reads them on its
 * own task. When the card goes away, fs_leaving() takes a card source out
 * before anything is unmounted.
 */

/*
 * USB hands reads over 4 KB at a time, and a card command per 4 KB costs more
 * than the data: 5.6 MB/s where the card reads 9. A read that goes straight on
 * takes this much from the card in one command.
 */
#define CARD_READ_AHEAD (256u * 1024u)

typedef struct {
    FIL f;
} card_file_t;

static esp_err_t card_file_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    card_file_t *cf = ctx;
    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    /*
     * Retry a failed read: this board occasionally returns a CRC error (0x109)
     * on an SD read at full speed, and such errors are transient - the same
     * block reads clean on the next try. Over a multi-gigabyte boot image the
     * rare miss would otherwise reach the target as a disk error. A handful of
     * attempts turns those into a slight hitch instead.
     */
    for (int attempt = 0; s_card && attempt < 4 && err != ESP_OK; attempt++) {
        UINT br = 0;
        if (f_lseek(&cf->f, (FSIZE_t)off) == FR_OK && f_read(&cf->f, buf, len, &br) == FR_OK &&
            br == len) {
            err = ESP_OK;
        }
    }
    xSemaphoreGive(s_card_lock);
    return err;
}

static void card_file_close(void *ctx)
{
    card_file_t *cf = ctx;
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    if (s_card) {
        f_close(&cf->f);
    }
    xSemaphoreGive(s_card_lock);
    free(cf);
}

static const kvm_media_ops_t s_card_file_ops = {
    .kind = "card",
    .read = card_file_read,
    .close = card_file_close,
    .read_ahead = CARD_READ_AHEAD,
};

esp_err_t kvm_storage_source_image(const char *name, bool cdrom, kvm_media_source_t *out)
{
    if (!s_card_lock || !name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Raw FATFS paths live on drive "0:"; reject any slashes so a setting can
     * only ever name a file in the card's root, never escape it. */
    if (strpbrk(name, "/\\")) {
        return ESP_ERR_INVALID_ARG;
    }
    card_file_t *cf = calloc(1, sizeof(*cf));
    if (!cf) {
        return ESP_ERR_NO_MEM;
    }
    char path[80];
    snprintf(path, sizeof(path), "0:/%s", name);
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    const FRESULT fr = s_card ? f_open(&cf->f, path, FA_READ) : FR_NOT_READY;
    const uint64_t size = fr == FR_OK ? (uint64_t)f_size(&cf->f) : 0;
    xSemaphoreGive(s_card_lock);
    if (fr != FR_OK) {
        free(cf);
        if (fr == FR_NOT_READY) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "cannot open image '%s' (FRESULT %d)", name, fr);
        return fr == FR_NO_FILE || fr == FR_NO_PATH ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    *out = (kvm_media_source_t){.ops = &s_card_file_ops, .ctx = cf, .size = size, .cdrom = cdrom};
    snprintf(out->name, sizeof(out->name), "%s", name);
    return ESP_OK;
}

/* The whole card as raw sectors. USB reads and writes are whole 512-byte
 * blocks, so offsets and lengths are sector-aligned. */
static esp_err_t whole_card_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    if (s_card && off % MEDIA_BLOCK_SIZE == 0 && len % MEDIA_BLOCK_SIZE == 0) {
        err = sdmmc_read_sectors(s_card, buf, (size_t)(off / MEDIA_BLOCK_SIZE), len / MEDIA_BLOCK_SIZE);
    }
    xSemaphoreGive(s_card_lock);
    return err;
}

#if SD_CAN_WRITE
static esp_err_t whole_card_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    if (s_card && off % MEDIA_BLOCK_SIZE == 0 && len % MEDIA_BLOCK_SIZE == 0) {
        err = sdmmc_write_sectors(s_card, buf, (size_t)(off / MEDIA_BLOCK_SIZE), len / MEDIA_BLOCK_SIZE);
    }
    xSemaphoreGive(s_card_lock);
    return err;
}
#endif

static const kvm_media_ops_t s_whole_card_ops = {
    .kind = "whole card",
    .read = whole_card_read,
#if SD_CAN_WRITE
    .write = whole_card_write, /* a pre-3.0 chip without the IO LDO times out on writes */
#endif
    .read_ahead = CARD_READ_AHEAD,
};

esp_err_t kvm_storage_source_whole_card(kvm_media_source_t *out)
{
    if (!s_card_lock || !s_card) {
        return ESP_ERR_INVALID_STATE; /* nothing to hand over */
    }
    fs_leaving("the whole card was handed to the target");
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    if (!s_card) {
        xSemaphoreGive(s_card_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* The card stays mounted for the firmware's own use, but the firmware keeps
     * off the filesystem from now on (uploads, the file list), so the target is
     * its only writer, until kvm_storage_reread(). */
    const uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    s_handed_over = true;
    xSemaphoreGive(s_card_lock);
    *out = (kvm_media_source_t){.ops = &s_whole_card_ops, .size = bytes};
    snprintf(out->name, sizeof(out->name), "whole card");
    return ESP_OK;
}

/* Memory-mapped flash: fast at any size and reliable, so no read-ahead or retry. */
static esp_err_t rescue_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    return esp_partition_read(s_rescue, (size_t)off, buf, len);
}

static const kvm_media_ops_t s_rescue_ops = {
    .kind = "rescue",
    .read = rescue_read,
};

esp_err_t kvm_storage_source_rescue(bool cdrom, kvm_media_source_t *out)
{
    if (!s_card_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_rescue) {
        return ESP_ERR_NOT_SUPPORTED; /* table predates the rescue partition */
    }
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    const bool writing = s_rescue_writing;
    xSemaphoreGive(s_card_lock);
    if (writing) {
        return ESP_ERR_INVALID_STATE; /* being re-flashed; cannot serve it now */
    }
    if (!rescue_has_image()) {
        return ESP_ERR_NOT_FOUND; /* partition is erased; nothing to boot */
    }
    /* The whole partition is offered as the disk. A raw boot image (an iPXE
     * .usb, a floppy) describes its real extent in its own MBR; the erased tail
     * past it is never read by a booting target. */
    *out = (kvm_media_source_t){.ops = &s_rescue_ops, .size = s_rescue->size, .cdrom = cdrom};
    snprintf(out->name, sizeof(out->name), "rescue");
    return ESP_OK;
}
void kvm_storage_rescue_status(kvm_rescue_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->supported = s_rescue != NULL;
    if (s_rescue) {
        out->capacity_bytes = s_rescue->size;
        out->has_image = rescue_has_image();
    }
}

esp_err_t kvm_storage_rescue_write_begin(size_t total)
{
    if (!s_rescue || !s_card_lock) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (total == 0 || total > s_rescue->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Stop serving the image while it changes underneath the target, and lock
     * out a second uploader. */
    kvm_media_eject_ops(&s_rescue_ops);
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    if (s_rescue_writing) {
        xSemaphoreGive(s_card_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_rescue_writing = true;
    s_rescue_wpos = 0;
    xSemaphoreGive(s_card_lock);

    /* Erase only what the image needs, rounded up to the 4 KB sector. */
    size_t erase = (total + 0xFFFu) & ~(size_t)0xFFFu;
    if (erase > s_rescue->size) {
        erase = s_rescue->size;
    }
    esp_err_t err = esp_partition_erase_range(s_rescue, 0, erase);
    if (err != ESP_OK) {
        xSemaphoreTake(s_card_lock, portMAX_DELAY);
        s_rescue_writing = false;
        xSemaphoreGive(s_card_lock);
        ESP_LOGE(TAG, "rescue erase failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "rescue upload started: %u bytes into a %u KB partition",
                 (unsigned)total, (unsigned)(s_rescue->size / 1024));
    }
    return err;
}

esp_err_t kvm_storage_rescue_write(const void *buf, size_t len)
{
    if (!s_rescue_writing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!buf || s_rescue_wpos + len > s_rescue->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_partition_write(s_rescue, s_rescue_wpos, buf, len);
    if (err == ESP_OK) {
        s_rescue_wpos += len;
    } else {
        ESP_LOGE(TAG, "rescue write at %u failed: %s", (unsigned)s_rescue_wpos,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t kvm_storage_rescue_write_end(void)
{
    if (!s_rescue_writing) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    s_rescue_writing = false;
    xSemaphoreGive(s_card_lock);
    ESP_LOGI(TAG, "rescue image written: %u bytes", (unsigned)s_rescue_wpos);
    return ESP_OK;
}

void kvm_storage_rescue_write_abort(void)
{
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    s_rescue_writing = false;
    xSemaphoreGive(s_card_lock);
}

bool kvm_storage_card_handed_over(void)
{
    return s_handed_over;
}

void kvm_storage_reread(void)
{
    if (!s_handed_over || !s_card_lock) {
        return;
    }
    /* The target had the card read-write; drop our now-stale filesystem view and
     * mount it fresh so we see whatever it wrote (or reformatted). fs_leaving
     * takes the medium out first, so no target read is left to touch the card
     * object as it is freed. */
    ESP_LOGI(TAG, "re-reading microSD after target write access");
    fs_leaving("the card is being re-read");
    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    s_handed_over = false;
    if (s_card) {
        (void)esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
    xSemaphoreGive(s_card_lock);
    (void)kvm_storage_init();
}

/* How many times a timed-out init is retried with a fresh power-cycle. A
 * seated card that fails the first cold start usually takes on the second; a
 * genuinely empty slot just costs a few power-cycles and a short delay. */
#define SD_MOUNT_ATTEMPTS 10

/*
 * The P4 has one SDMMC controller with two slots. Slot 0 has dedicated pins;
 * slot 1 goes through the GPIO matrix, and it is the slot esp-hosted uses for a
 * WiFi co-processor. The driver's default is slot 1, so the card and the C6
 * used to fight over it. A card on the slot 0 pins takes slot 0 and the two
 * work side by side. The driver picks the dedicated pins by itself when the
 * numbers match.
 */
int kvm_storage_sd_slot(void)
{
#if SOC_SDMMC_USE_IOMUX
    if (KVM_BOARD_SD_CLK_GPIO == SDMMC_SLOT0_IOMUX_PIN_NUM_CLK &&
        KVM_BOARD_SD_CMD_GPIO == SDMMC_SLOT0_IOMUX_PIN_NUM_CMD &&
        KVM_BOARD_SD_D0_GPIO == SDMMC_SLOT0_IOMUX_PIN_NUM_D0 &&
        KVM_BOARD_SD_D1_GPIO == SDMMC_SLOT0_IOMUX_PIN_NUM_D1 &&
        KVM_BOARD_SD_D2_GPIO == SDMMC_SLOT0_IOMUX_PIN_NUM_D2 &&
        KVM_BOARD_SD_D3_GPIO == SDMMC_SLOT0_IOMUX_PIN_NUM_D3) {
        return SDMMC_HOST_SLOT_0;
    }
#endif
    return SDMMC_HOST_SLOT_1;
}

bool kvm_storage_shares_wifi_slot(void)
{
#if CONFIG_KVM_WIFI && defined(CONFIG_ESP_HOSTED_HOST_SDIO_SLOT)
    return kvm_storage_sd_slot() == CONFIG_ESP_HOSTED_HOST_SDIO_SLOT;
#else
    return false;
#endif
}

/* Win back a step while the card is idle. Holds the media lock so the card is
 * not unmounted under the test; a target read waits the second it takes. */
/*
 * Is a card still in the slot? A card that was pulled out answers nothing, so
 * one sector, asked twice, tells them apart. The read is deliberately invisible:
 * it does not slow the bus on failure and does not count as the card being used,
 * or every poll would postpone the speed climb that waits for an idle card.
 */
static bool sd_present(void)
{
    static uint8_t *s_probe_buf;
    if (!s_probe_buf) {
        s_probe_buf = heap_caps_aligned_alloc(64, MEDIA_BLOCK_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_probe_buf) {
            return true; /* no memory to ask with; assume the card is there */
        }
    }
    const int64_t io = s_last_io_us;
    s_presence_poll = true;
    bool here = false;
    for (int i = 0; !here && i < 2; i++) {
        here = sdmmc_read_sectors(s_card, s_probe_buf, 0, 1) == ESP_OK;
    }
    s_presence_poll = false;
    s_last_io_us = io;
    return here;
}

/* The one line that says a card is up: the boot prints it, so does a card that
 * appeared in the slot later. */
static void sd_log_mounted(void)
{
    kvm_storage_status_t st;
    kvm_storage_status(&st);
    ESP_LOGI(TAG, "mounted %s: %llu MB total, %llu MB free, bus %lu kHz", st.name,
             (unsigned long long)(st.total_bytes / (1024 * 1024)),
             (unsigned long long)(st.free_bytes / (1024 * 1024)), (unsigned long)st.bus_khz);
}

/*
 * The slot's card-detect switch, where a board wires one (CONFIG_KVM_SD_CD_GPIO;
 * low with a card in unless CONFIG_KVM_SD_CD_ACTIVE_HIGH - the M5Stack add-on's
 * TF_DET on G37 reads high with a card in, against the internal pull-up). A change on it wakes
 * the slot check at once instead of at the next five-second tick, and an empty
 * slot is not tried. Without it the check simply asks the card, as before.
 */
static TaskHandle_t s_probe_task;

#if CONFIG_KVM_SD_CD_GPIO >= 0
static void IRAM_ATTR sd_cd_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (s_probe_task) {
        vTaskNotifyGiveFromISR(s_probe_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void sd_cd_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_KVM_SD_CD_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* The internal pull-up either way. On the M5Stack add-on the switch is
           open with a card in and closes to ground without one, and nothing
           else holds the line: left floating it read 0 - no card - with a card
           in, and so did an internal pull-down. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = kvm_board_gpio_isr_service();
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(CONFIG_KVM_SD_CD_GPIO, sd_cd_isr, NULL);
    }
    ESP_LOGI(TAG, "card detect on GPIO %d%s", CONFIG_KVM_SD_CD_GPIO,
             err == ESP_OK ? "" : " - not usable, checking by asking the card");
}
#endif

/* Whether the slot's switch says a card is in; true where there is no switch. */
static bool sd_cd_says_card(void)
{
#if CONFIG_KVM_SD_CD_GPIO >= 0
#if CONFIG_KVM_SD_CD_ACTIVE_HIGH
    return gpio_get_level(CONFIG_KVM_SD_CD_GPIO) == 1;
#else
    return gpio_get_level(CONFIG_KVM_SD_CD_GPIO) == 0;
#endif
#else
    return true;
#endif
}

/* The switch read once at boot, before the watcher sets its interrupt up. */
static bool sd_cd_boot_says_card(void)
{
#if CONFIG_KVM_SD_CD_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_KVM_SD_CD_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* as in sd_cd_init() */
    };
    if (gpio_config(&io) != ESP_OK) {
        return true; /* ask the card instead */
    }
    esp_rom_delay_us(100); /* let the pull-up charge the line */
#endif
    return sd_cd_says_card();
}

static void sd_probe_task(void *arg)
{
    (void)arg;
    s_probe_task = xTaskGetCurrentTaskHandle();
#if CONFIG_KVM_SD_CD_GPIO >= 0
    sd_cd_init();
#endif
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) {
            /* The switch moved. Let a card going in finish seating, and let the
               check run now even if the card was just in use. */
            vTaskDelay(pdMS_TO_TICKS(300));
            (void)ulTaskNotifyTake(pdTRUE, 0);
            s_slot_recheck = true;
        }
        const int64_t now = esp_timer_get_time();
        /*
         * The slot, watched. A card pushed in after boot should work without a
         * restart, and one pulled out should stop being offered to the target.
         * Only while the SD host is ours and either the card is idle or a real
         * transfer just failed, so this never lands in the middle of the
         * target's reads. A card handed to the target whole is still checked -
         * it is one sector, and a card that left the slot has to disappear from
         * the target too.
         */
        if (!s_bus_released && s_card_lock &&
            (s_slot_recheck || now - s_last_io_us >= SD_PROBE_IDLE_US) &&
            xSemaphoreTake(s_card_lock, 0) == pdTRUE) {
            s_slot_recheck = false;
            bool changed = false;
            if (!s_card && !s_handed_over && !s_other_slot_busy && sd_cd_says_card()) {
                /*
                 * One quiet try; the next tick is five seconds away. Not while
                 * the co-processor holds the other slot: mounting re-initialises
                 * the SD host both slots share, so on a board running WiFi a card
                 * put in after boot waits for the next restart. Losing one is
                 * still noticed - that is a read, not a re-init.
                 */
                if (sd_mount(1) == ESP_OK) {
                    while (!sd_read_test(SD_MOUNT_TEST_SECTORS) && s_bus_khz > SD_FLOOR_KHZ) {
                    }
                    sd_log_mounted();
                    changed = true;
                }
            } else if (s_card && (!sd_cd_says_card() || !sd_present())) {
                ESP_LOGW(TAG, "microSD is gone; the slot reads as empty now");
                /* Files open on the card are closed before it is unmounted. */
                xSemaphoreGive(s_card_lock);
                fs_leaving("the microSD card was removed");
                xSemaphoreTake(s_card_lock, portMAX_DELAY);
                s_handed_over = false; /* nothing left to hand anyone */
                if (s_card) {
                    (void)esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
                    s_card = NULL;
                }
                s_next_probe_us = 0;
                changed = true;
            }
            xSemaphoreGive(s_card_lock);
            if (changed && s_slot_cb) {
                /* Re-offers the chosen image, or shows the drive as empty. */
                s_slot_cb();
            }
        }
        if (s_climbed_us && now - s_climbed_us > (int64_t)SD_PROBE_MAX_S * 1000000) {
            s_probe_wait_s = SD_PROBE_FIRST_S; /* that speed held; start patient again */
            s_climbed_us = 0;
        }
        if (!s_next_probe_us || now < s_next_probe_us || now - s_last_io_us < SD_PROBE_IDLE_US ||
            s_other_slot_busy || !s_card_lock) {
            continue;
        }
        if (xSemaphoreTake(s_card_lock, 0) != pdTRUE) {
            continue;
        }
        if (s_card && !s_handed_over && s_bus_khz < s_start_khz) {
            const uint32_t from = s_bus_khz;
            const uint32_t to = sd_step_above(from);
            s_bus_khz = to;
            (void)sdmmc_host_set_card_clk(s_card->host.slot, to);
            s_probing = true;
            const bool clean = sd_read_test(SD_PROBE_SECTORS);
            s_probing = false;
            if (clean) {
                s_climbed_us = esp_timer_get_time();
                s_next_probe_us =
                    to < s_start_khz ? s_climbed_us + (int64_t)SD_PROBE_FIRST_S * 1000000 : 0;
                ESP_LOGW(TAG, "microSD bus back up to %lu kHz", (unsigned long)to);
            } else {
                /* The hook stepped it down; a test that fails is the same as a
                 * failure right after a climb. */
                s_probe_wait_s =
                    s_probe_wait_s * 2 < SD_PROBE_MAX_S ? s_probe_wait_s * 2 : SD_PROBE_MAX_S;
                s_next_probe_us = esp_timer_get_time() + (int64_t)s_probe_wait_s * 1000000;
                ESP_LOGW(TAG, "microSD still fails at %lu kHz; next try in %lu s",
                         (unsigned long)to, (unsigned long)s_probe_wait_s);
            }
        } else if (!s_card || s_bus_khz >= s_start_khz) {
            s_next_probe_us = 0;
        }
        xSemaphoreGive(s_card_lock);
    }
}

/*
 * Bring the card up: bus, slot, mount, with the clock ladder. @p attempts is how
 * many tries it gets - the boot gives it ten, the slot watcher one, because it
 * comes back in five seconds anyway.
 */
/*
 * The sdmmc driver, the FAT layer and the GPIO driver all shout when a mount
 * finds nothing. That is the normal answer for an empty slot, and the watcher
 * asks every five seconds, so the shouting is turned off for the length of a
 * quiet try - about five lines a tick that would otherwise fill the log ring.
 */
static void sd_driver_logs(esp_log_level_t level)
{
    static const char *const tags[] = {"vfs_fat_sdmmc", "sdmmc_common", "sdmmc_sd",
                                       "sdmmc_io",      "SD_HOST",      "gpio"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], level);
    }
}

static esp_err_t sd_mount(int attempts)
{
    /* One attempt is the slot watcher; the boot gets its usual noise. */
    const bool quiet = attempts == 1;
    if (quiet) {
        sd_driver_logs(ESP_LOG_NONE);
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = kvm_storage_sd_slot();
    /*
     * Stay on 3.3 V; never negotiate UHS-I. UHS-I switches the card to 1.8 V and
     * only removing its power switches it back, so on a board with no power gate
     * for the card every warm restart (an update, a panic) leaves a 1.8 V card on
     * a 3.3 V bus. SDR50 ran on the Function EV, and that is where it ended.
     */
    host.is_slot_set_to_uhs1 = NULL;
    /* No DDR either: double-data-rate clocking proved flaky on this slot. */
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
    /*
     * On some boards the slot's IO pins are powered by one of the chip's LDOs
     * (VDD_IO_5 from LDO_VO4 on the Function EV). Left off, the card still reads
     * at a low clock, but writes fail CRC at 4 MHz and reads fail above that.
     */
#if CONFIG_KVM_SD_IO_LDO_CHAN >= 0
    static sd_pwr_ctrl_handle_t s_io_ldo;
    if (!s_io_ldo) {
        const sd_pwr_ctrl_ldo_config_t ldo_cfg = {.ldo_chan_id = CONFIG_KVM_SD_IO_LDO_CHAN};
        esp_err_t lerr = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_io_ldo);
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "microSD IO power (LDO %d): %s", CONFIG_KVM_SD_IO_LDO_CHAN,
                     esp_err_to_name(lerr));
        }
    }
    host.pwr_ctrl_handle = s_io_ldo;
#endif
    /*
     * A card that reads and will not write happens too: a 256 GB SDXC card
     * mounted and read here but failed every write (ESP_ERR_INVALID_CRC, FIFO
     * underrun 0xe00) while a 32 GB card wrote fine on the same board.
     */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = KVM_BOARD_SD_CLK_GPIO;
    slot.cmd = KVM_BOARD_SD_CMD_GPIO;
    slot.d0 = KVM_BOARD_SD_D0_GPIO;
    slot.d1 = KVM_BOARD_SD_D1_GPIO;
    slot.d2 = KVM_BOARD_SD_D2_GPIO;
    slot.d3 = KVM_BOARD_SD_D3_GPIO;
    /* The board carries external pull-ups; the internal ones are enabled too as
     * a belt-and-suspenders, harmless where the externals already hold. */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        /* Never reformat a card that will not mount: it may be the operator's,
         * with data on it, and a KVM has no business wiping it to make itself
         * tidy. A card that does not mount is reported empty instead. */
        .format_if_mount_failed = false,
        /* A recording and its subtitles, two downloads or a player, an upload
         * and a disk image; the file table lives in PSRAM. */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = ESP_FAIL;
    s_start_khz = sd_start_khz();
    s_bus_khz = s_start_khz;
    s_bus_errors = 0;
    s_next_probe_us = 0;
    s_climbed_us = 0;
    s_probe_wait_s = SD_PROBE_FIRST_S;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        /* Two failures at one clock step it down; the floor keeps what is left. */
        if (attempt > 1 && attempt % 2 == 1 && s_bus_khz > SD_FLOOR_KHZ) {
            s_bus_khz = sd_step_below(s_bus_khz);
        }
        host.max_freq_khz = (int)s_bus_khz;
        /* Let power settle before each attempt; a retry after an intermittent
         * failure gets a fresh settle too. */
        slot_power_settle();
        err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount, &s_card);
        if (err == ESP_OK) {
            break;
        }
        s_card = NULL;
        /* Retry every failure, not just timeouts: this slot answers commands
         * intermittently, so a card that gives an invalid response on one
         * attempt often initialises cleanly on the next. A genuinely absent card
         * simply times out every attempt and is reported empty below. */
        if (attempts > 1) {
            /* A single attempt is the slot watcher asking an empty slot; saying
             * so every five seconds would fill the log with nothing. */
            ESP_LOGI(TAG, "mount attempt %d/%d at %lu kHz failed: %s", attempt, attempts,
                     (unsigned long)s_bus_khz, esp_err_to_name(err));
        }
    }

    if (quiet) {
        sd_driver_logs(CONFIG_LOG_DEFAULT_LEVEL);
    }
    return err;
}

esp_err_t kvm_storage_init(void)
{
    if (!s_card_lock) {
        s_card_lock = xSemaphoreCreateMutex();
    }
    /* The built-in rescue image lives here; independent of the card, so this is
     * found whether or not a card ever mounts. Absent on an older table. */
    if (!s_rescue) {
        s_rescue = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "rescue");
        if (s_rescue) {
            ESP_LOGI(TAG, "rescue partition: %lu KB, %s", (unsigned long)(s_rescue->size / 1024),
                     rescue_has_image() ? "image present" : "empty");
        }
    }
    slot_power_claim();

    /* An empty slot by its switch is not tried: ten timeouts would only fill
     * the boot log. The watcher mounts a card that goes in later. */
    const esp_err_t err = sd_cd_boot_says_card() ? sd_mount(SD_MOUNT_ATTEMPTS) : ESP_ERR_NOT_FOUND;
    s_bus_released = false;

    /* Watches the slot whether or not a card is in it right now. */
    static bool s_probe_started;
    if (!s_probe_started) {
        s_probe_started =
            xTaskCreate(sd_probe_task, "sd_probe", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS;
    }

    if (err != ESP_OK) {
        /* An empty or unreadable slot is a normal state, not a start-up
         * failure: the device is a KVM first and a virtual drive second. */
        ESP_LOGI(TAG, "no card in the slot");
        return ESP_OK;
    }

    /* The test read steps the bus down on its own when it fails; stop once a
     * pass is clean or there is nothing slower to try. */
    while (!sd_read_test(SD_MOUNT_TEST_SECTORS) && s_bus_khz > SD_FLOOR_KHZ) {
    }
    sd_log_mounted();
    return ESP_OK;
}

esp_err_t kvm_storage_bus_suspend(bool *was_mounted)
{
    if (was_mounted) {
        *was_mounted = false;
    }
    if (!s_card) {
        return ESP_OK; /* nothing mounted - the SD host controller is already free */
    }
    /* fs_leaving pulls a medium on the card before the filesystem goes away. */
    fs_leaving("the SD host was handed to the WiFi chip");
    xSemaphoreTake(s_card_lock, portMAX_DELAY); /* not under a speed probe */
    s_handed_over = false; /* the card is going away; the remount on resume is fresh */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_next_probe_us = 0;
    s_bus_released = true; /* the slot watcher must keep its hands off it now */
    xSemaphoreGive(s_card_lock);
    if (was_mounted) {
        *was_mounted = true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bus suspend unmount: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "microSD unmounted - SD host controller handed over");
    }
    return err;
}

esp_err_t kvm_storage_bus_resume(void)
{
    /* Re-mount from scratch: kvm_storage_init re-claims the slot and mounts,
     * retrying the intermittent slot the same way it does at boot. */
    ESP_LOGI(TAG, "reacquiring the SD host controller for the microSD");
    return kvm_storage_init();
}
