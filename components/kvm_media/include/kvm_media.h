/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual media: the one USB drive the target sees, and what is behind it.
 *
 * Behind it is a source: an image file on the microSD, the whole card, the
 * built-in rescue image, an image on a web server, or a file on the
 * operator's own computer, served by a console tab. Every source is read the same way, through one
 * cache, on one task of its own. The USB stack hands a read over and is free
 * at once, so keys and the mouse never wait for a disk or a network.
 *
 * One medium at a time. Inserting another closes the one before.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a source does. Its calls run on the media task, one at a time. */
typedef struct {
    /** Short name for the log: "card", "whole card", "rescue", "URL", "browser". */
    const char *kind;
    /**
     * Read exactly @p len bytes at @p off. The media layer never asks past
     * the end of the source. ESP_OK, or an error: the target sees a read error.
     * ESP_ERR_NOT_FOUND says the medium itself is gone (the tab serving it was
     * closed): the media layer then ejects it, and the target sees the drive
     * empty instead. ESP_ERR_TIMEOUT says the source tried for as long as it
     * may: the read is not tried again. A read should end well within the
     * host's own timeout (30 s on Linux and Windows), or the host resets the drive.
     */
    esp_err_t (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    /** Write, the same way. NULL for a source the target may only read. */
    esp_err_t (*write)(void *ctx, uint64_t off, const void *buf, uint32_t len);
    /** Called once, after the last read, when the medium leaves. May be NULL. */
    void (*close)(void *ctx);
    /**
     * Called when the target has read nothing for a while, so a network source
     * can let its connection go and give the memory back. May be NULL.
     */
    void (*idle)(void *ctx);
    /**
     * Called from another task just before the medium is taken out, while a
     * read of it may be waiting on the network. It makes that read give up at
     * once, so the eject does not wait for it. Must not block. May be NULL for
     * a source whose reads are quick.
     */
    void (*cancel)(void);
    /**
     * How much to read in one go when the target reads straight on, in bytes.
     * 0: read what is asked, for a source that is fast at any size.
     */
    uint32_t read_ahead;
} kvm_media_ops_t;

/** A source ready to be inserted. */
typedef struct {
    const kvm_media_ops_t *ops;
    void *ctx;
    uint64_t size;     /**< bytes */
    bool cdrom;        /**< served as a CD-ROM (2048-byte blocks), else a disk (512) */
    char name[64];     /**< shown in the log and to the target's medium-change check */
} kvm_media_source_t;

/** The medium as the target sees it. */
typedef struct {
    bool present;
    bool writable;
    bool cdrom;
    uint32_t block_size;  /**< 2048 for a CD-ROM, 512 for a disk */
    uint64_t block_count;
    char name[64];        /**< empty when nothing is inserted */
} kvm_media_t;

/** A read or write that did not complete, as the result passed to the callback. */
enum {
    KVM_MEDIA_ERR_IO = -1,        /**< the source failed */
    KVM_MEDIA_ERR_NO_MEDIUM = -2, /**< nothing inserted, or it left meanwhile */
};

/** Called on the media task with the bytes done, or a KVM_MEDIA_ERR_*, and the caller's @p arg. */
typedef void (*kvm_media_done_t)(int32_t result, void *arg);

/** Start the media task. Safe to call more than once. */
esp_err_t kvm_media_init(void);

/**
 * Insert @p src, closing what was there. The media layer owns the source from
 * here and calls its close() when it leaves. A source smaller than one block
 * is refused with ESP_ERR_INVALID_SIZE and closed.
 */
esp_err_t kvm_media_insert(const kvm_media_source_t *src);

/** Take the medium out. The target sees an empty drive. */
void kvm_media_eject(void);

/**
 * Take the medium out only if it is a source with these @p ops. The owner of a
 * source calls this when what is behind it goes away, a card pulled out for
 * one. Returns once no read of it is running, and its close() was called.
 */
void kvm_media_eject_ops(const kvm_media_ops_t *ops);

/** True while a source with these @p ops is inserted. Never waits for a read. */
bool kvm_media_is(const kvm_media_ops_t *ops);

/** The medium as the target sees it. Never waits for a read, so the USB task
 *  can ask it at any time. */
void kvm_media_info(kvm_media_t *out);

/** Whether the target read or wrote the medium in the last @p ms: the status
    screen turns its disc while it does. */
bool kvm_media_busy(uint32_t ms);

/**
 * Read @p len bytes at byte @p off into @p buf, on the media task. @p buf must
 * stay valid until @p done is called. Past the end of the medium the bytes are
 * zeros, so a whole block always comes back.
 * @return ESP_OK when queued, then @p done reports the result.
 */
esp_err_t kvm_media_read_async(uint64_t off, void *buf, uint32_t len, kvm_media_done_t done, void *arg);

/** The same for a write. A read-only medium reports KVM_MEDIA_ERR_IO. */
esp_err_t kvm_media_write_async(uint64_t off, const void *buf, uint32_t len, kvm_media_done_t done,
                                void *arg);

#ifdef __cplusplus
}
#endif
