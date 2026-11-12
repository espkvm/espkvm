/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The GPIO ISR service is one per chip, but several drivers need it. A second
 * gpio_install_isr_service() returns ESP_ERR_INVALID_STATE and IDF logs that as
 * an error, so every driver asks here instead.
 */
#include "kvm_board.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_installed;

esp_err_t kvm_board_gpio_isr_service(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool done = s_installed;
    s_installed = true;
    taskEXIT_CRITICAL(&s_lock);
    if (done) {
        return ESP_OK;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK; /* installed outside our drivers */
    }
    if (err != ESP_OK) {
        s_installed = false;
    }
    return err;
}
