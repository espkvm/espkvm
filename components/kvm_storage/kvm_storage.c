/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_storage.h"

#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "kvm_board.h"
#include "kvm_caps.h"

#define TAG "storage"

#define MOUNT_POINT "/sd"

static sdmmc_card_t *s_card;

/*
 * The slot power gate. Q1 passes 3.3 V to the card when this pin is driven to
 * its active level; leaving it idle powers the card down. A powered-down slot
 * mounts nothing and gives no error worth the name, so this is done first and
 * checked nowhere - if it fails, the mount below fails loudly enough.
 */
static void slot_power_on(void)
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
        return;
    }
    gpio_set_level(KVM_BOARD_SD_PWR_GPIO, KVM_BOARD_SD_PWR_ACTIVE_LOW ? 0 : 1);
    /* The MOSFET and the card's own regulator need a moment before the bus is
     * worth talking to. */
    vTaskDelay(pdMS_TO_TICKS(10));
}

const char *kvm_storage_mount_point(void)
{
    return MOUNT_POINT;
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

esp_err_t kvm_storage_init(void)
{
    slot_power_on();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

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

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "no card in the slot");
        } else {
            ESP_LOGW(TAG, "card present but not mounted: %s", esp_err_to_name(err));
        }
        /* An empty or unreadable slot is a normal state, not a start-up
         * failure: the device is a KVM first and a virtual drive second. */
        return ESP_OK;
    }

    kvm_storage_status_t st;
    kvm_storage_status(&st);
    ESP_LOGI(TAG, "mounted %s: %llu MB total, %llu MB free", st.name,
             (unsigned long long)(st.total_bytes / (1024 * 1024)),
             (unsigned long long)(st.free_bytes / (1024 * 1024)));
    return ESP_OK;
}
