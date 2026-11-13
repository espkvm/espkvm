/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The menu on the status screen, worked with the buttons on the box. All of it
 * runs on the button task.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Open the menu at its first item (the "menu" button action). */
void kvm_menu_open(void);

bool kvm_menu_is_open(void);

/** The next item, wrapping around. */
void kvm_menu_next(void);

/** Carry out the item shown and close the menu. Back only closes it. */
void kvm_menu_pick(void);

/** Close the menu once it has gone unused long enough. Call it often. */
void kvm_menu_tick(void);

#ifdef __cplusplus
}
#endif
