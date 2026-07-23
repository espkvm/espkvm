/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board wiring. The pins live in Kconfig (menuconfig -> ESP-KVM -> Board pins,
 * and Network for Ethernet) with the Waveshare ESP32-P4-ETH values as defaults,
 * so porting to another ESP32-P4 board is a menuconfig edit rather than a code
 * change. This header only gives those Kconfig values their in-code names; the
 * few constants that are not board pins stay here directly. See docs/PORTING.md.
 */
#pragma once

#include "sdkconfig.h"

/* Not pins: the P4's internal LDO that powers the MIPI PHY, and the capture
 * geometry. These are chip- or capture-level and rarely change per board. */
#define KVM_BOARD_MIPI_LDO_CHAN_ID 3
#define KVM_BOARD_MIPI_LDO_VOLTAGE_MV 2500
#define KVM_BOARD_CSI_H_RES 1920u
#define KVM_BOARD_CSI_V_RES 1080u
#define KVM_BOARD_MIPI_LANE_MBPS 972

/*
 * The BOOT button, identified by probing the board rather than by reading a
 * datasheet: idle high through its pull-up, pulled to ground when pressed. It
 * is also the chip's boot strapping pin, so holding it while the board powers
 * up lands in the ROM downloader instead of this firmware - which is why the
 * password reset watches for a long press while running, not at start-up. Set
 * to -1 (Kconfig) on a board with no reachable button; the feature is then off.
 */
#define KVM_BOARD_BUTTON_GPIO CONFIG_KVM_BUTTON_GPIO

#define KVM_BOARD_TC358743_I2C_SDA_GPIO CONFIG_KVM_I2C_SDA_GPIO
#define KVM_BOARD_TC358743_I2C_SCL_GPIO CONFIG_KVM_I2C_SCL_GPIO

#define KVM_BOARD_TC358743_REFCLK_HZ ((unsigned)CONFIG_KVM_TC358743_REFCLK_HZ)

/*
 * microSD on a 4-bit SDIO slot. The Waveshare defaults come from Waveshare's own
 * SDMMC example for this board and are verified on hardware.
 *
 * The power-gate GPIO is not a data line: it gates the MOSFET that passes 3.3 V
 * to the card (active-low on the Waveshare). Leave it high and the slot is
 * unpowered, which looks exactly like an empty slot. On a board whose slot is
 * always powered, set the power-gate GPIO to -1 and the firmware leaves the pin
 * alone.
 */
#define KVM_BOARD_SD_CLK_GPIO CONFIG_KVM_SD_CLK_GPIO
#define KVM_BOARD_SD_CMD_GPIO CONFIG_KVM_SD_CMD_GPIO
#define KVM_BOARD_SD_D0_GPIO CONFIG_KVM_SD_D0_GPIO
#define KVM_BOARD_SD_D1_GPIO CONFIG_KVM_SD_D1_GPIO
#define KVM_BOARD_SD_D2_GPIO CONFIG_KVM_SD_D2_GPIO
#define KVM_BOARD_SD_D3_GPIO CONFIG_KVM_SD_D3_GPIO
#define KVM_BOARD_SD_PWR_GPIO CONFIG_KVM_SD_PWR_GPIO
#ifdef CONFIG_KVM_SD_PWR_ACTIVE_LOW
#define KVM_BOARD_SD_PWR_ACTIVE_LOW 1
#else
#define KVM_BOARD_SD_PWR_ACTIVE_LOW 0
#endif
