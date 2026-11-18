/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reading Telegram's replies without a JSON tree. getUpdates can be 100 KB,
 * and a cJSON tree of it would be thousands of small blocks in internal RAM.
 * Plain C with no IDF headers, so it runs in a host test.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pick the chats out of a getUpdates reply (NUL-terminated) and write them to
 * @p out as a JSON array: [{"id":"-100123","type":"group","name":"Ops"}].
 * Each chat once, in the order it first appears, at most @p max_chats.
 * Returns how many, or -1 if @p out is too small.
 */
int tg_chats_from_updates(const char *json, char *out, size_t cap, int max_chats);

/** The bot's username from a getMe reply, as JSON-escaped text. */
bool tg_bot_username(const char *json, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
