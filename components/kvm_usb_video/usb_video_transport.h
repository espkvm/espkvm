/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_VIDEO_RX_FIFO_SIZE 8192
#define USB_VIDEO_TX_FIFO_SIZE 2048

/* Only the USB task may mutate the transport. */
void usb_video_transport_attach(uint8_t *rx_buffer, uint8_t *tx_buffer);
void usb_video_transport_detach(void);
void usb_video_transport_reset_session(void);
size_t usb_video_transport_read(uint8_t *data, size_t size);
size_t usb_video_transport_write(const uint8_t *data, size_t size);

/* The receiver uses these snapshots to avoid waking the USB task to poll. */
bool usb_video_transport_mounted(void);
bool usb_video_transport_pending(void);

/* Implemented by the receiver. Called from the USB task. */
void usb_video_transport_event(bool reset);
