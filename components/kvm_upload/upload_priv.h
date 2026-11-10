/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inside kvm_upload: what a destination is. The core (kvm_upload.c) owns the
 * queue and the task; a destination knows whether it is set up and how to
 * put one file. WebDAV is the first; S3 would be the next file beside it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How a put went, and what to tell the operator when it did not. */
typedef struct {
    bool ok;
    bool retry; /* the server could not be reached or had a moment: try again later */
    char message[96];
} upload_res_t;

/* Called as a file goes out; returns false to stop it (cancelled). */
typedef bool (*upload_progress_fn)(uint64_t bytes);

typedef struct {
    const char *name; /* "WebDAV", for the console */
    bool (*ready)(void);
    /* @p full is the file's path on the card's file system, @p rel its path
       from the card's root, which the destination keeps. */
    upload_res_t (*put)(const char *full, const char *rel, uint64_t size, upload_progress_fn progress);
    /* Reach the server and make the device's folder there. */
    upload_res_t (*test)(void);
} upload_dest_t;

extern const upload_dest_t upload_dest_dav;
