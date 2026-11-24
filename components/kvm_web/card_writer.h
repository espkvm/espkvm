/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Write a stream to a file on the card while the next part arrives: the network
 * side fills a buffer, a second task writes it. A card write and a network read
 * take about as long, so doing them one after the other halves the speed.
 * Buffers are PSRAM, cache-line aligned, so the SD driver sends each in one DMA
 * multi-block command. When all buffers are queued, getting the next one waits,
 * and TCP slows the sender.
 */
typedef struct card_writer card_writer_t;

/* Start writing to @p fd (left open; the caller closes it). @p tag names the
 * writer task and the log lines. NULL when there is no memory for it. */
card_writer_t *card_writer_start(int fd, const char *tag);

/* The size of each buffer, smaller than the default when PSRAM is in pieces. */
size_t card_writer_chunk(const card_writer_t *w);

/* An empty buffer of card_writer_chunk() bytes; waits for one to come back. */
char *card_writer_get(card_writer_t *w);

/* Hand a filled buffer over to be written; @p len 0 just gives it back. */
void card_writer_put(card_writer_t *w, char *buf, size_t len);

/* True once a write has failed (card full, removed). Later buffers are dropped. */
bool card_writer_failed(const card_writer_t *w);

/* Wait for the queued writes, free everything. @p write_us (may be NULL) gets
 * the time spent writing. @return false if any write failed. */
bool card_writer_finish(card_writer_t *w, int64_t *write_us);
