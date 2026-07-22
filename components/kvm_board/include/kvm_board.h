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
