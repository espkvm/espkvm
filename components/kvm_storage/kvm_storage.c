/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_storage.h"

#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "kvm_board.h"
#include "kvm_caps.h"

#define TAG "storage"

#define MOUNT_POINT "/sd"
#define MEDIA_BLOCK_SIZE 512u

/*
 * The card is served read-only. This board's SD interface is marginal (a known
 * ESP32-P4 limitation): it reads reliably only at a low clock (see the bus setup
 * below) and cannot write at all - write commands time out getting status at
 * every clock the read path can use, with no software knob for the write-side
 * timing. So images are prepared in an external card reader and the device only
 * reads. This lets the web layer disable upload/delete with a plain reason
 * instead of letting them fail.
 */
#define SD_WRITE_UNAVAILABLE_REASON \
    "this board cannot write the microSD reliably; prepare the card in a reader"

static sdmmc_card_t *s_card;

/* ---- virtual media --------------------------------------------------------
 *
 * The inserted image, guarded by a mutex because the target reads it from the
 * USB task while selection happens on the web task. Raw FATFS is used rather
 * than stdio: f_lseek takes a 32-bit FSIZE_t that spans the whole 4 GiB FAT32
 * file range, where fseek's long offset would overflow past 2 GiB. Drive "0:"
 * is the same volume esp_vfs_fat mounted, so f_getfree above and f_read here
 * see one filesystem. */
static SemaphoreHandle_t s_media_lock;
static FIL s_media_file;
static bool s_media_open;
static uint64_t s_media_blocks;
static char s_media_name[64];

#define SD_PWR_ON_LEVEL (KVM_BOARD_SD_PWR_ACTIVE_LOW ? 0 : 1)

static void slot_power_claim(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << KVM_BOARD_SD_PWR_GPIO,
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
    gpio_set_level(KVM_BOARD_SD_PWR_GPIO, SD_PWR_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(20));
}

const char *kvm_storage_mount_point(void)
{
    return MOUNT_POINT;
}

bool kvm_storage_writable(void)
{
    return false;
}

const char *kvm_storage_write_unavailable_reason(void)
{
    return SD_WRITE_UNAVAILABLE_REASON;
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

    /* Capacity comes from the card; free space from the filesystem. FATFS
     * reports in clusters, so the two multiply back up to bytes. */
    out->total_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs) {
        const uint64_t cluster_bytes = (uint64_t)(fs->csize) * s_card->csd.sector_size;
        out->free_bytes = (uint64_t)free_clusters * cluster_bytes;
    }
}

/* ---- virtual media API ---------------------------------------------------- */

static void media_close_locked(void)
{
    if (s_media_open) {
        f_close(&s_media_file);
        s_media_open = false;
    }
    s_media_blocks = 0;
    s_media_name[0] = '\0';
}

esp_err_t kvm_storage_media_select(const char *name)
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
    FSIZE_t size = f_size(&s_media_file);
    if (size < MEDIA_BLOCK_SIZE) {
        f_close(&s_media_file);
        xSemaphoreGive(s_media_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    s_media_open = true;
    s_media_blocks = size / MEDIA_BLOCK_SIZE;
    snprintf(s_media_name, sizeof(s_media_name), "%s", name);
    xSemaphoreGive(s_media_lock);
    ESP_LOGI(TAG, "media inserted: '%s', %llu blocks (%llu MB)", name,
             (unsigned long long)s_media_blocks,
             (unsigned long long)((uint64_t)size / (1024 * 1024)));
    return ESP_OK;
}

void kvm_storage_media_eject(void)
{
    (void)kvm_storage_media_select(NULL);
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
    /*
     * Retry a failed read: this board occasionally returns a CRC error (0x109)
     * on an SD read at full speed, and such errors are transient - the same
     * block reads clean on the next try. Over a multi-gigabyte boot image the
     * rare miss would otherwise reach the target as a disk error. A handful of
     * attempts turns those into a slight hitch instead.
     */
    int32_t got = -1;
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
    if (got < 0) {
        ESP_LOGW(TAG, "media read failed at offset %llu after retries",
                 (unsigned long long)offset);
    }
    xSemaphoreGive(s_media_lock);
    return got;
}

/* How many times a timed-out init is retried with a fresh power-cycle. A
 * seated card that fails the first cold start usually takes on the second; a
 * genuinely empty slot just costs a few power-cycles and a short delay. */
#define SD_MOUNT_ATTEMPTS 10

esp_err_t kvm_storage_init(void)
{
    if (!s_media_lock) {
        s_media_lock = xSemaphoreCreateMutex();
    }
    slot_power_claim();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /*
     * Stay on 3.3 V high-speed; never negotiate UHS-I.
     *
     * UHS-I means switching the bus to 1.8 V (CMD11) and running SDR50/104 - the
     * most signal-integrity-sensitive part of init, and one this board does not
     * do reliably: the card would time out on SEND_OP_COND, or answer and then
     * fail the voltage switch (0x107 / 0x108), intermittently, so the slot
     * looked empty on most boots. The driver only requests the 1.8 V switch when
     * this callback says the slot supports it; clearing it keeps init on the
     * robust 3.3 V path. A KVM serving a boot image does not need UHS speed.
     */
    host.is_slot_set_to_uhs1 = NULL;
    /* No DDR either: double-data-rate clocking is the other speed feature this
     * slot proved flaky on. High-speed SDR is plenty for boot media. */
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
    /*
     * Cap the bus at 4 MHz. At the 20 MHz default this board reads single
     * sectors (mount, directory) fine but fails EVERY multi-block read - which
     * is what a USB host does when it reads the disk, so a target could not read
     * the image at all. At 4 MHz bulk reads succeed: a host enumerates the drive
     * and reads its partition table cleanly. ~2 MB/s is slow but it works, where
     * full speed did not work at all. (Writes are still unreliable even here, so
     * the card stays read-only - see kvm_storage_writable.)
     */
    /*
     * Cap the bus at 4 MHz (40 MHz / 10 - the P4 clock is integer fractions of
     * 40 MHz). At the 20 MHz default this board reads single sectors (mount,
     * directory) but fails EVERY multi-block read, which is what a USB host does,
     * so a target could not read the image at all; 8 MHz still drops some. Input
     * delay phase tuning did not help at any phase - the ceiling is the board's
     * SD signal integrity, a known ESP32-P4 limitation. At 4 MHz bulk reads are
     * clean: verified by a host reading 200 MB with zero errors (~1.5 MB/s).
     */
    host.max_freq_khz = 4000;

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
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
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
        ESP_LOGI(TAG, "mount attempt %d/%d failed: %s", attempt, SD_MOUNT_ATTEMPTS,
                 esp_err_to_name(err));
    }

    if (err != ESP_OK) {
        /* An empty or unreadable slot is a normal state, not a start-up
         * failure: the device is a KVM first and a virtual drive second. */
        ESP_LOGI(TAG, "no card in the slot");
        return ESP_OK;
    }

    kvm_storage_status_t st;
    kvm_storage_status(&st);
    ESP_LOGI(TAG, "mounted %s: %llu MB total, %llu MB free", st.name,
             (unsigned long long)(st.total_bytes / (1024 * 1024)),
             (unsigned long long)(st.free_bytes / (1024 * 1024)));
    return ESP_OK;
}
