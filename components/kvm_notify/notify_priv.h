/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inside kvm_notify: the waiting event as the channels see it, and what a
 * channel is. The core (kvm_notify.c) owns the queue, the pending list and its
 * copy on the card. A channel only knows whether it is set up and how to send
 * one event.
 */
#pragma once

#include "kvm_notify.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define NOTIFY_TITLE_MAX 80
#define NOTIFY_BODY_MAX 200
#define NOTIFY_HTTP_TIMEOUT_MS 15000
/* A screen JPEG we are willing to attach. Bigger than this and we send text
   only - a notification is not a place for a megabyte. */
#define NOTIFY_PHOTO_MAX (2 * 1024 * 1024) /* a 1080p screenshot at quality 90 */
/* What a screen JPEG usually takes, for the check before one is made. The
   worst case above was asked for before, which a board with under 5 MB of
   PSRAM free never had, so it never sent a picture. The real size is checked
   once the JPEG exists. */
#define NOTIFY_PHOTO_TYPICAL (512 * 1024)
/* How much of the tail of the device log to attach, when asked. Telegram takes
   up to 4096 characters in one message; a webhook takes it as a field. */
#define NOTIFY_LOG_TAIL_MAX 2048
/* Kept free in PSRAM when a screenshot comes back from the card for a send. */
#define NOTIFY_PSRAM_RESERVE (3 * 1024 * 1024)

typedef enum {
    EV_SEND,
    EV_FIND_CHATS,
    EV_CLIP,
} notify_ev_op_t;

typedef struct {
    notify_ev_op_t op;
    kvm_notify_ev_t kind;
    char title[NOTIFY_TITLE_MAX];
    char body[NOTIFY_BODY_MAX];
    bool want_photo;
    char path[112];     /* EV_CLIP: the file */
    char card_path[80]; /* EV_CLIP: how the card names it */
    char only[16];      /* a test for one channel: its route key; "" = every channel */
} notify_event_t;

/* How a send went: sent, failed in a way worth trying again (no network, a
   timeout, the server busy or rate-limiting), or refused (a 4xx), which a
   retry would only repeat. */
typedef enum {
    SEND_OK,
    SEND_RETRY,
    SEND_REFUSED,
} send_res_t;

#define NOTIFY_CH_MAX 8

/* An event waiting for its channels, with what it carried when it happened. */
typedef struct {
    notify_event_t ev;
    time_t at;      /* wall clock when it happened, 0 if the clock was unset */
    int64_t at_us;  /* uptime when it happened */
    uint8_t *photo; /* PSRAM, or NULL */
    size_t photo_len;
    char *logtail;  /* PSRAM, or NULL */
    uint32_t left[NOTIFY_CH_MAX]; /* per channel, what it still has to reach (0 = done) */
    uint32_t id;      /* its files on the card are .notify/<id>.* (0 = memory only) */
    bool photo_file;  /* the screenshot is on the card, not in memory */
    bool log_file;
    bool before_boot; /* read back from the card after a restart */
} notify_pending_t;

/*
 * A place notifications go. targets() says what the channel can reach right
 * now: 0 when it is not set up, else a bit per destination (one bit for a
 * chat or a URL, a bit per browser for Web Push). send() tries one event,
 * clears the bits in *left that are done with (sent, refused, gone) and
 * returns true when something refused it.
 */
typedef struct {
    const char *id;        /* its key in a waiting event's file on the card */
    const char *route_key; /* the setting with the mask of kinds it takes */
    bool photo;            /* can carry a screenshot: one is taken only for these */
    uint32_t (*targets)(void);
    bool (*send)(const notify_pending_t *p, uint32_t *left);
} notify_channel_t;

extern const notify_channel_t notify_channel_tg;
extern const notify_channel_t notify_channel_hook;
extern const notify_channel_t notify_channel_push;
extern const notify_channel_t notify_channel_ntfy;

/* --- shared by the channels, from kvm_notify.c ------------------------------ */

send_res_t notify_classify(int status);
void notify_url_encode(const char *src, char *dst, size_t cap);
void notify_json_escape(const char *src, char *dst, size_t cap);
/* The message as a person reads it: title, body, and when a late one happened. */
void notify_compose(const notify_pending_t *p, char *text, size_t cap);
/* The screenshot or log tail, from memory or back from the card. Free *loaded
   after use; NULL when there is none. */
const uint8_t *notify_photo(const notify_pending_t *p, size_t *len, uint8_t **loaded);
const char *notify_logtail(const notify_pending_t *p, char **loaded);
/* Text for a clip on a channel that cannot carry the file itself. */
void notify_clip_text(const notify_pending_t *p, char *text, size_t cap);
/* Post an event onto the notify task's queue; false when it is full. */
bool notify_post(const notify_event_t *ev);

/* --- Telegram's chat finder, run on the notify task (notify_tg.c) ------------ */
void notify_tg_find_chats(void);
void notify_tg_init(void);
