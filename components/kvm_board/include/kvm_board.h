/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#define KVM_BOARD_MIPI_LDO_CHAN_ID 3
#define KVM_BOARD_MIPI_LDO_VOLTAGE_MV 2500
/*
 * The BOOT button, identified by probing the board rather than by reading a
 * datasheet: idle high through its pull-up, pulled to ground when pressed.
 * It is also the chip's boot strapping pin, so holding it while the board
 * powers up lands in the ROM downloader instead of this firmware - which is
 * why the password reset watches for a long press while running, not at
 * start-up.
 */
#define KVM_BOARD_BUTTON_GPIO 35

#define KVM_BOARD_TC358743_I2C_SDA_GPIO 7
#define KVM_BOARD_TC358743_I2C_SCL_GPIO 8

#define KVM_BOARD_TC358743_REFCLK_HZ 27000000u

#define KVM_BOARD_CSI_H_RES 1920u
#define KVM_BOARD_CSI_V_RES 1080u

#define KVM_BOARD_MIPI_LANE_MBPS 972

/*
 * microSD on a 4-bit SDIO 3.0 slot. These pins are from Waveshare's own SDMMC
 * example for this board and confirmed working on hardware by a third party
 * (the stock SDMMC_Test runs unmodified); the project's own bring-up verifies
 * them by mounting a real card and reading a file back.
 *
 * GPIO 45 is not a data line: it gates Q1, the MOSFET that passes 3.3 V to the
 * card, and it is active-low. Leave it high and the slot is unpowered, which
 * looks exactly like an empty slot - a card that will not mount for no visible
 * reason. Drive it low before touching the bus.
 */
#define KVM_BOARD_SD_CLK_GPIO 43
#define KVM_BOARD_SD_CMD_GPIO 44
#define KVM_BOARD_SD_D0_GPIO 39
#define KVM_BOARD_SD_D1_GPIO 40
#define KVM_BOARD_SD_D2_GPIO 41
#define KVM_BOARD_SD_D3_GPIO 42
#define KVM_BOARD_SD_PWR_GPIO 45
#define KVM_BOARD_SD_PWR_ACTIVE_LOW 1
