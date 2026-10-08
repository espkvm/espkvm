/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_storage.h"

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

/* Outside the media lock: whoever listens may take a while to close its files,
 * and a target read must not wait behind that. */
static void fs_leaving(const char *why)
{
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

/* ---- virtual media --------------------------------------------------------
 *
 * The inserted image, guarded by a mutex because the target reads it from the
 * USB task while selection happens on the web task. Raw FATFS is used rather
 * than stdio: f_lseek takes a 64-bit FSIZE_t that covers exFAT files past
 * 4 GiB, where fseek's long offset would overflow past 2 GiB. Drive "0:"
 * is the same volume esp_vfs_fat mounted, so f_getfree above and f_read here
 * see one filesystem. */
static SemaphoreHandle_t s_media_lock;
static FIL s_media_file;
static bool s_media_open;
static uint64_t s_media_blocks;
static uint32_t s_media_bsize = MEDIA_BLOCK_SIZE; /* 512 for a disk, 2048 for a CD */
static bool s_media_cdrom;
/* True while the whole card is handed to the target read-write: the firmware then
 * keeps off the filesystem so there is a single writer, and re-reads the card
 * (kvm_storage_reread) when the operator switches the medium back. */
static bool s_handed_over;
static char s_media_name[64];

/*
 * What the inserted image is backed by. The target sees one USB drive; behind
 * it is either a file on the microSD card or the on-flash "rescue" partition.
 * Both coexist - the operator picks which one is inserted - so booting from the
 * card is unaffected by the built-in image, and the built-in image is there
 * even with no card in the slot.
 */
typedef enum { MEDIA_SRC_NONE, MEDIA_SRC_SD, MEDIA_SRC_FLASH, MEDIA_SRC_WHOLE_SD } media_src_t;
static media_src_t s_media_src;
/* The rescue partition, found once at init; NULL on a device whose partition
 * table predates it (it is served only when present). */
static const esp_partition_t *s_rescue;
/* An upload in progress: the partition is being erased/written, so it must not
 * be served to the target meanwhile. The flag is read from other tasks (select,
 * begin) under s_media_lock, so every transition of it is made under the lock
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

/* ---- virtual media API ---------------------------------------------------- */

/*
 * Read-ahead for the target's reads. USB hands them over 4 KB at a time, and a
 * card command per 4 KB costs more than the data: 5.6 MB/s where the card reads
 * 9. So a read that follows the last one takes 256 KB from the card in one
 * command, into PSRAM, and the next reads are served from there. A bigger USB
 * buffer does the same from the other side, but it lives in internal RAM, which
 * WiFi needs, and moved to PSRAM the USB transfers get slower instead. A read that
 * jumps elsewhere - a file system looking at its tables - goes straight to the
 * card at its own size, so random access does not pay for data it never uses.
 * Any write to the medium drops the buffer.
 */
#define MEDIA_RA_BYTES (256u * 1024u)
static uint8_t *s_ra_buf;
static uint64_t s_ra_off;  /* medium offset of s_ra_buf[0] */
static uint32_t s_ra_len;  /* valid bytes, 0 when empty */
static uint64_t s_ra_next; /* where a sequential read would start */

static void media_close_locked(void)
{
    s_ra_len = 0;
    s_ra_next = 0;
    if (s_media_src == MEDIA_SRC_SD && s_media_open) {
        f_close(&s_media_file);
    }
    s_media_open = false;
    s_media_src = MEDIA_SRC_NONE;
    s_media_blocks = 0;
    s_media_bsize = MEDIA_BLOCK_SIZE;
    s_media_cdrom = false;
    s_media_name[0] = '\0';
}

esp_err_t kvm_storage_media_select(const char *name, bool cdrom)
{
    if (!s_media_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    media_close_locked();

    if (!name || !name[0]) { /* eject */
        xSemaphoreGive(s_media_lock);
        ESP_LOGI(TAG, "media ejected");
        return ESP_OK;
    }
    if (!s_card) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* Raw FATFS paths live on drive "0:"; reject any slashes so a setting can
     * only ever name a file in the card's root, never escape it. */
    if (strpbrk(name, "/\\")) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_ARG;
    }
    char path[80];
    snprintf(path, sizeof(path), "0:/%s", name);

    FRESULT fr = f_open(&s_media_file, path, FA_READ);
    if (fr != FR_OK) {
        xSemaphoreGive(s_media_lock);
        ESP_LOGW(TAG, "cannot open image '%s' (FRESULT %d)", name, fr);
        return fr == FR_NO_FILE || fr == FR_NO_PATH ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    const uint32_t bsize = cdrom ? 2048u : MEDIA_BLOCK_SIZE;
    FSIZE_t size = f_size(&s_media_file);
    if (size < bsize) {
        f_close(&s_media_file);
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    s_media_open = true;
    s_media_src = MEDIA_SRC_SD;
    s_media_bsize = bsize;
    s_media_cdrom = cdrom;
    s_media_blocks = size / bsize;
    snprintf(s_media_name, sizeof(s_media_name), "%s", name);
    xSemaphoreGive(s_media_lock);
    ESP_LOGI(TAG, "media inserted: '%s' as %s, %llu blocks (%llu MB)", name,
             cdrom ? "CD-ROM" : "disk", (unsigned long long)s_media_blocks,
             (unsigned long long)((uint64_t)size / (1024 * 1024)));
    return ESP_OK;
}

esp_err_t kvm_storage_media_select_rescue(bool cdrom)
{
    if (!s_media_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    if (s_rescue_writing) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_STATE; /* being re-flashed; cannot serve it now */
    }
    media_close_locked();
    if (!s_rescue) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_NOT_SUPPORTED; /* table predates the rescue partition */
    }
    if (!rescue_has_image()) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_NOT_FOUND; /* partition is erased; nothing to boot */
    }
    /* The whole partition is offered as the disk. A raw boot image (an iPXE
     * .usb, a floppy) describes its real extent in its own MBR; the erased tail
     * past it is never read by a booting target. */
    s_media_open = true;
    s_media_src = MEDIA_SRC_FLASH;
    s_media_bsize = cdrom ? 2048u : MEDIA_BLOCK_SIZE;
    s_media_cdrom = cdrom;
    s_media_blocks = s_rescue->size / s_media_bsize;
    snprintf(s_media_name, sizeof(s_media_name), "rescue");
    xSemaphoreGive(s_media_lock);
    ESP_LOGI(TAG, "media inserted: built-in rescue image as %s, %llu blocks",
             cdrom ? "CD-ROM" : "disk", (unsigned long long)s_media_blocks);
    return ESP_OK;
}

esp_err_t kvm_storage_media_select_whole_sd(void)
{
    if (!s_media_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_card) {
        fs_leaving("the whole card was handed to the target");
    }
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    media_close_locked();
    if (!s_card) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_STATE; /* nothing to hand over */
    }
    /* Serve the card's raw sectors. The card stays mounted for the firmware's own
     * use (uploads, the file list); reads go through the SDMMC host, which
     * serialises transactions, so this coexists with FATFS access - though a write
     * from the console mid-read would make the target's view momentarily stale, it
     * is read-only to the target so the card is never corrupted. */
    const uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    s_media_open = true;
    s_media_src = MEDIA_SRC_WHOLE_SD;
    s_media_bsize = MEDIA_BLOCK_SIZE;
    s_media_cdrom = false;
    s_media_blocks = bytes / MEDIA_BLOCK_SIZE;
    s_handed_over = true; /* the target now owns the card; firmware stays off the FS */
    snprintf(s_media_name, sizeof(s_media_name), "whole card");
    xSemaphoreGive(s_media_lock);
    ESP_LOGI(TAG, "media inserted: whole microSD card, %llu blocks (%llu MB)",
             (unsigned long long)s_media_blocks, (unsigned long long)(bytes / (1024 * 1024)));
    return ESP_OK;
}

void kvm_storage_media_eject(void)
{
    (void)kvm_storage_media_select(NULL, false);
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
    if (!s_rescue || !s_media_lock) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (total == 0 || total > s_rescue->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Stop serving the image while it changes underneath the target, and lock
     * out a second uploader. */
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    if (s_rescue_writing) {
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_media_src == MEDIA_SRC_FLASH) {
        media_close_locked();
    }
    s_rescue_writing = true;
    s_rescue_wpos = 0;
    xSemaphoreGive(s_media_lock);

    /* Erase only what the image needs, rounded up to the 4 KB sector. */
    size_t erase = (total + 0xFFFu) & ~(size_t)0xFFFu;
    if (erase > s_rescue->size) {
        erase = s_rescue->size;
    }
    esp_err_t err = esp_partition_erase_range(s_rescue, 0, erase);
    if (err != ESP_OK) {
        xSemaphoreTake(s_media_lock, portMAX_DELAY);
        s_rescue_writing = false;
        xSemaphoreGive(s_media_lock);
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
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    s_rescue_writing = false;
    xSemaphoreGive(s_media_lock);
    ESP_LOGI(TAG, "rescue image written: %u bytes", (unsigned)s_rescue_wpos);
    return ESP_OK;
}

void kvm_storage_rescue_write_abort(void)
{
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    s_rescue_writing = false;
    xSemaphoreGive(s_media_lock);
}

void kvm_storage_media_info(kvm_media_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->block_size = MEDIA_BLOCK_SIZE;
    if (!s_media_lock) {
        return;
    }
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    out->present = s_media_open;
    out->writable = false;
    out->cdrom = s_media_cdrom;
    out->block_size = s_media_bsize;
    out->block_count = s_media_blocks;
    snprintf(out->name, sizeof(out->name), "%s", s_media_name);
    xSemaphoreGive(s_media_lock);
}

int32_t kvm_storage_media_read(uint64_t offset, void *buf, uint32_t len)
{
    if (!buf || !s_media_lock) {
        return -1;
    }
    /* A read of the last block can run past end-of-file; hand the host zeros
     * for the tail rather than a short or failed transfer. */
    memset(buf, 0, len);
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    if (!s_media_open) {
        xSemaphoreGive(s_media_lock);
        return -1;
    }

    int32_t got = -1;
    const bool sequential = offset == s_ra_next;
    s_ra_next = offset + len;
    if (s_media_src != MEDIA_SRC_FLASH && s_ra_len && offset >= s_ra_off &&
        offset + len <= s_ra_off + s_ra_len) {
        memcpy(buf, s_ra_buf + (offset - s_ra_off), len);
        got = (int32_t)len;
    } else if (s_media_src != MEDIA_SRC_FLASH && sequential && len <= MEDIA_RA_BYTES &&
               (s_ra_buf || (s_ra_buf = heap_caps_aligned_alloc(64, MEDIA_RA_BYTES,
                                                               MALLOC_CAP_SPIRAM)) != NULL)) {
        s_ra_len = 0;
        if (s_media_src == MEDIA_SRC_WHOLE_SD) {
            if (s_card && (offset % MEDIA_BLOCK_SIZE) == 0 && (len % MEDIA_BLOCK_SIZE) == 0) {
                const uint64_t first = offset / MEDIA_BLOCK_SIZE;
                const uint64_t left = s_card->csd.capacity > first ? s_card->csd.capacity - first : 0;
                const uint32_t sectors = (uint32_t)(left < MEDIA_RA_BYTES / MEDIA_BLOCK_SIZE
                                                        ? left
                                                        : MEDIA_RA_BYTES / MEDIA_BLOCK_SIZE);
                if (sectors * MEDIA_BLOCK_SIZE >= len &&
                    sdmmc_read_sectors(s_card, s_ra_buf, (size_t)first, sectors) == ESP_OK) {
                    s_ra_len = sectors * MEDIA_BLOCK_SIZE;
                }
            }
        } else {
            UINT br = 0;
            if (f_lseek(&s_media_file, (FSIZE_t)offset) == FR_OK &&
                f_read(&s_media_file, s_ra_buf, MEDIA_RA_BYTES, &br) == FR_OK) {
                s_ra_len = br;
                if (br < len) {
                    /* The end of the file: hand over what there is, zero-filled,
                     * and keep nothing - a partial block is not worth serving. */
                    memcpy(buf, s_ra_buf, br);
                    s_ra_len = 0;
                    got = (int32_t)len;
                }
            }
        }
        if (s_ra_len) {
            s_ra_off = offset;
            memcpy(buf, s_ra_buf, len);
            got = (int32_t)len;
        }
    }

    /* Not served from the read-ahead, or it failed: the read itself. */
    if (got < 0 && s_media_src == MEDIA_SRC_WHOLE_SD) {
        /* Raw whole-card passthrough. USB reads are always whole 512-byte blocks,
         * so the offset and length are sector-aligned; sdmmc_read_sectors goes
         * straight to the card, bypassing the filesystem. */
        if (s_card && (offset % MEDIA_BLOCK_SIZE) == 0 && (len % MEDIA_BLOCK_SIZE) == 0 &&
            sdmmc_read_sectors(s_card, buf, (size_t)(offset / MEDIA_BLOCK_SIZE),
                               len / MEDIA_BLOCK_SIZE) == ESP_OK) {
            got = (int32_t)len;
        }
    } else if (got < 0 && s_media_src == MEDIA_SRC_FLASH) {
        /* Memory-mapped flash: fast and reliable, no retry needed. A read past
         * the end of the partition keeps the zero fill from the memset above. */
        uint32_t avail = 0;
        if (offset < s_rescue->size) {
            const uint64_t rest = s_rescue->size - offset;
            avail = rest < len ? (uint32_t)rest : len;
        }
        if (avail == 0 || esp_partition_read(s_rescue, (size_t)offset, buf, avail) == ESP_OK) {
            got = (int32_t)len;
        }
    } else if (got < 0) {
        /*
         * Retry a failed read: this board occasionally returns a CRC error
         * (0x109) on an SD read at full speed, and such errors are transient -
         * the same block reads clean on the next try. Over a multi-gigabyte boot
         * image the rare miss would otherwise reach the target as a disk error.
         * A handful of attempts turns those into a slight hitch instead.
         */
        for (int attempt = 0; attempt < 4 && got < 0; attempt++) {
            if (f_lseek(&s_media_file, (FSIZE_t)offset) != FR_OK) {
                continue;
            }
            UINT br = 0;
            if (f_read(&s_media_file, buf, len, &br) == FR_OK) {
                got = (int32_t)len; /* zero-padded above, so the block is complete */
                (void)br;
            }
        }
    }
    if (got < 0) {
        ESP_LOGW(TAG, "media read failed at offset %llu after retries",
                 (unsigned long long)offset);
    }
    xSemaphoreGive(s_media_lock);
    return got;
}

bool kvm_storage_media_writable(void)
{
#if SD_CAN_WRITE
    if (!s_media_lock) {
        return false;
    }
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    /* Only the whole-card passthrough is writable; images and CD-ROM stay
     * read-only (a booting target must not corrupt the operator's image). */
    const bool w = s_media_open && s_media_src == MEDIA_SRC_WHOLE_SD && s_card != NULL;
    xSemaphoreGive(s_media_lock);
    return w;
#else
    return false; /* pre-3.0 without the IO LDO: writes time out */
#endif
}

int32_t kvm_storage_media_write(uint64_t offset, const void *buf, uint32_t len)
{
    if (!buf || !s_media_lock) {
        return -1;
    }
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    s_ra_len = 0; /* the read-ahead may hold these sectors */
    int32_t done = -1;
    /* Raw whole-card writes only. USB writes are whole 512-byte blocks, so the
     * offset and length are sector-aligned. */
    if (s_media_open && s_media_src == MEDIA_SRC_WHOLE_SD && s_card &&
        (offset % MEDIA_BLOCK_SIZE) == 0 && (len % MEDIA_BLOCK_SIZE) == 0 &&
        sdmmc_write_sectors(s_card, buf, (size_t)(offset / MEDIA_BLOCK_SIZE),
                            len / MEDIA_BLOCK_SIZE) == ESP_OK) {
        done = (int32_t)len;
    }
    xSemaphoreGive(s_media_lock);
    if (done < 0) {
        ESP_LOGW(TAG, "media write failed at offset %llu", (unsigned long long)offset);
    }
    return done;
}

bool kvm_storage_card_handed_over(void)
{
    return s_handed_over;
}

void kvm_storage_reread(void)
{
    if (!s_handed_over || !s_media_lock) {
        return;
    }
    /* The target had the card read-write; drop our now-stale filesystem view and
     * mount it fresh so we see whatever it wrote (or reformatted). Close the
     * medium and unmount under the lock first, so a target read in flight cannot
     * touch the card object as it is freed. */
    ESP_LOGI(TAG, "re-reading microSD after target write access");
    fs_leaving("the card is being re-read");
    xSemaphoreTake(s_media_lock, portMAX_DELAY);
    s_handed_over = false;
    media_close_locked();
    if (s_card) {
        (void)esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
    xSemaphoreGive(s_media_lock);
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
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
        }
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
        if (!s_bus_released && s_media_lock &&
            (s_slot_recheck || now - s_last_io_us >= SD_PROBE_IDLE_US) &&
            xSemaphoreTake(s_media_lock, 0) == pdTRUE) {
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
                xSemaphoreGive(s_media_lock);
                fs_leaving("the microSD card was removed");
                xSemaphoreTake(s_media_lock, portMAX_DELAY);
                media_close_locked();
                s_handed_over = false; /* nothing left to hand anyone */
                if (s_card) {
                    (void)esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
                    s_card = NULL;
                }
                s_ra_len = 0;
                s_next_probe_us = 0;
                changed = true;
            }
            xSemaphoreGive(s_media_lock);
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
            s_other_slot_busy || !s_media_lock) {
            continue;
        }
        if (xSemaphoreTake(s_media_lock, 0) != pdTRUE) {
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
        xSemaphoreGive(s_media_lock);
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
    if (!s_media_lock) {
        s_media_lock = xSemaphoreCreateMutex();
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

    const esp_err_t err = sd_mount(SD_MOUNT_ATTEMPTS);
    s_bus_released = false;

    /* Watches the slot whether or not a card is in it right now. */
    static bool s_probe_started;
    if (!s_probe_started) {
        s_probe_started =
            xTaskCreate(sd_probe_task, "sd_probe", 6144, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS;
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
    /* Pull any image the target is reading before the filesystem goes away. */
    fs_leaving("the SD host was handed to the WiFi chip");
    kvm_storage_media_eject();
    xSemaphoreTake(s_media_lock, portMAX_DELAY); /* not under a speed probe */
    s_handed_over = false; /* the card is going away; the remount on resume is fresh */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_next_probe_us = 0;
    s_bus_released = true; /* the slot watcher must keep its hands off it now */
    xSemaphoreGive(s_media_lock);
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
