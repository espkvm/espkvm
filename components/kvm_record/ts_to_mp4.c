/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ts_to_mp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS 188
#define PID_VIDEO 0x0100
#define PID_AUDIO 0x0101
#define TIMESCALE 90000u
#define AAC_FRAMES 1024
#define OPUS_FRAMES 960 /* ts_mux takes 20 ms packets */
#define OPUS_RATE 48000u
#define OPUS_CHANNELS 2 /* what ts_mux's table says */
/* What a decoder drops from the start: libopus's look-ahead at 48 kHz. */
#define OPUS_PRE_SKIP 312

/* ---- reading the transport stream --------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t len;
    size_t cap;
    int64_t pts;
    bool audio;
} au_buf_t;

typedef struct {
    const mp4_io_t *io;
    uint8_t pkt[TS];
    au_buf_t cur;   /* the access unit being gathered */
    au_buf_t acur;  /* the sound frame being gathered */
    au_buf_t ready; /* the last one handed out */
    size_t awant;   /* acur's length, from its PES header */
    bool replay;    /* pkt is read again: it ended the video unit before it */
    bool open;
    bool aopen;
    bool eof;
} ts_reader_t;

static bool au_append(au_buf_t *a, const uint8_t *p, size_t n)
{
    if (a->len + n > a->cap) {
        /* Small steps from a small start: on a board with a megabyte of PSRAM
         * left, a doubled buffer is the allocation that fails. */
        size_t cap = a->cap ? a->cap + a->cap / 2 : 64 * 1024;
        while (cap < a->len + n) {
            cap *= 2;
        }
        uint8_t *grown = realloc(a->b, cap);
        if (!grown) {
            return false;
        }
        a->b = grown;
        a->cap = cap;
    }
    memcpy(a->b + a->len, p, n);
    a->len += n;
    return true;
}

/* A gathered unit becomes the ready one; the old ready buffer is reused. */
static void hand_out(ts_reader_t *r, au_buf_t *from)
{
    const au_buf_t t = r->ready;
    r->ready = *from;
    *from = t;
    from->len = 0;
}

static void reader_free(ts_reader_t *r)
{
    free(r->cur.b);
    free(r->acur.b);
    free(r->ready.b);
}

/* An audio packet. 1 = a sound frame is complete, 0 = not yet, -1 = out of memory.
 * Its PES packet has a length, so it is done without waiting for the next one. */
static int audio_packet(ts_reader_t *r, const uint8_t *p, size_t at)
{
    if (p[1] & 0x40) {
        const uint8_t *h = p + at;
        r->aopen = false;
        if (TS - at < 14 || h[0] || h[1] || h[2] != 1 || !(h[7] & 0x80)) {
            return 0;
        }
        const size_t pes_len = ((size_t)h[4] << 8) | h[5];
        if (pes_len < 3u + h[8]) {
            return 0;
        }
        r->acur.len = 0;
        r->acur.audio = true;
        r->acur.pts = ((int64_t)(h[9] & 0x0e) << 29) | ((int64_t)h[10] << 22) |
                      ((int64_t)(h[11] & 0xfe) << 14) | ((int64_t)h[12] << 7) | (h[13] >> 1);
        r->awant = pes_len - 3 - h[8];
        r->aopen = true;
        at += 9 + h[8];
    }
    if (!r->aopen || at >= TS) {
        return 0;
    }
    size_t n = TS - at;
    if (n > r->awant - r->acur.len) {
        n = r->awant - r->acur.len;
    }
    if (!au_append(&r->acur, p + at, n)) {
        return -1;
    }
    if (r->acur.len < r->awant) {
        return 0;
    }
    r->aopen = false;
    hand_out(r, &r->acur);
    return 1;
}

static bool read_packet(ts_reader_t *r)
{
    size_t have = 0;
    while (have < TS) {
        const size_t n = r->io->read(r->io->io, r->pkt + have, TS - have);
        if (!n) {
            break;
        }
        have += n;
    }
    return have == TS && r->pkt[0] == 0x47;
}

/* The next access unit or sound frame in r->ready. 1 = got one, 0 = end, -1 = out
 * of memory. */
static int next_au(ts_reader_t *r)
{
    for (;;) {
        if (r->replay) {
            r->replay = false;
        } else if (r->eof || !read_packet(r)) {
            r->eof = true;
            if (r->open) {
                r->open = false;
                hand_out(r, &r->cur);
                return 1;
            }
            return 0;
        }
        const uint8_t *p = r->pkt;
        const int pid = ((p[1] & 0x1f) << 8) | p[2];
        if ((pid != PID_VIDEO && pid != PID_AUDIO) || !(p[3] & 0x10)) {
            continue;
        }
        size_t at = 4;
        if (p[3] & 0x20) {
            at = 5 + p[4];
            if (at >= TS) {
                continue;
            }
        }
        if (pid == PID_AUDIO) {
            /* ts_mux never puts sound inside a picture, so the picture before
             * it is whole: out it goes first, and the files keep their order. */
            if ((p[1] & 0x40) && r->open) {
                r->open = false;
                hand_out(r, &r->cur);
                r->replay = true;
                return 1;
            }
            const int a = audio_packet(r, p, at);
            if (a) {
                return a;
            }
            continue;
        }
        if (!(p[1] & 0x40)) {
            if (r->open && !au_append(&r->cur, p + at, TS - at)) {
                return -1;
            }
            continue;
        }
        /* A PES packet starts: whatever was gathered before it is complete. */
        const uint8_t *h = p + at;
        if (TS - at < 14 || h[0] || h[1] || h[2] != 1) {
            continue;
        }
        int64_t pts = 0;
        if (h[7] & 0x80) {
            pts = ((int64_t)(h[9] & 0x0e) << 29) | ((int64_t)h[10] << 22) |
                  ((int64_t)(h[11] & 0xfe) << 14) | ((int64_t)h[12] << 7) | (h[13] >> 1);
        }
        const size_t payload = at + 9 + h[8];
        const bool had = r->open;
        if (had) {
            hand_out(r, &r->cur);
        }
        r->open = true;
        r->cur.pts = pts;
        r->cur.audio = false;
        if (payload < TS && !au_append(&r->cur, p + payload, TS - payload)) {
            return -1;
        }
        if (had) {
            return 1;
        }
    }
}

/* ---- H.264 --------------------------------------------------------------------- */

typedef void (*nal_fn)(void *user, const uint8_t *nal, size_t len);

/* Call @p fn for each NAL unit in an Annex-B access unit. */
static void each_nal(const uint8_t *b, size_t n, nal_fn fn, void *user)
{
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                if (end > start && b[end - 1] == 0) {
                    end--; /* the leading zero of a four-byte start code */
                }
                fn(user, b + start, end - start);
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < n) {
        fn(user, b + start, n - start);
    }
}

typedef struct {
    size_t size; /* the sample, length-prefixed */
    bool idr;
    uint8_t *sps, *pps;
    size_t sps_len, pps_len;
    const mp4_io_t *io; /* writing pass only */
    bool ok;
} nal_scan_t;

static bool keep_nal(uint8_t type)
{
    /* The access unit delimiter and parameter sets do not go in the samples:
     * the parameter sets live in the sample description instead. */
    return type != 9 && type != 7 && type != 8;
}

static void scan_nal(void *user, const uint8_t *nal, size_t len)
{
    nal_scan_t *s = user;
    if (!len) {
        return;
    }
    const uint8_t type = nal[0] & 0x1f;
    if (type == 5) {
        s->idr = true;
    }
    if (type == 7 && !s->sps && len >= 4) {
        s->sps = malloc(len);
        if (s->sps) {
            memcpy(s->sps, nal, len);
            s->sps_len = len;
        }
    }
    if (type == 8 && !s->pps) {
        s->pps = malloc(len);
        if (s->pps) {
            memcpy(s->pps, nal, len);
            s->pps_len = len;
        }
    }
    if (keep_nal(type)) {
        s->size += 4 + len;
    }
}

static void write_nal(void *user, const uint8_t *nal, size_t len)
{
    nal_scan_t *s = user;
    if (!len || !s->ok || !keep_nal(nal[0] & 0x1f)) {
        return;
    }
    const uint8_t hdr[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    s->ok = s->io->write(s->io->io, hdr, 4) && s->io->write(s->io->io, nal, len);
}

/* ---- boxes --------------------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t len;
    size_t cap;
    bool bad;
} buf_t;

static void put(buf_t *o, const uint8_t *p, size_t n)
{
    if (o->bad) {
        return;
    }
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n) {
            cap *= 2;
        }
        uint8_t *g = realloc(o->b, cap);
        if (!g) {
            o->bad = true;
            return;
        }
        o->b = g;
        o->cap = cap;
    }
    memcpy(o->b + o->len, p, n);
    o->len += n;
}

static void u8(buf_t *o, uint8_t v) { put(o, &v, 1); }
static void u16(buf_t *o, uint16_t v) { const uint8_t b[2] = {v >> 8, v}; put(o, b, 2); }
static void u32(buf_t *o, uint32_t v)
{
    const uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    put(o, b, 4);
}
static void u64(buf_t *o, uint64_t v) { u32(o, (uint32_t)(v >> 32)); u32(o, (uint32_t)v); }
static void zeros(buf_t *o, size_t n) { while (n--) u8(o, 0); }

/* Open a box: its size is filled in by box_end. */
static size_t box(buf_t *o, const char *type)
{
    const size_t at = o->len;
    u32(o, 0);
    put(o, (const uint8_t *)type, 4);
    return at;
}

static void box_end(buf_t *o, size_t at)
{
    if (o->bad) {
        return;
    }
    const uint32_t n = (uint32_t)(o->len - at);
    o->b[at] = n >> 24;
    o->b[at + 1] = n >> 16;
    o->b[at + 2] = n >> 8;
    o->b[at + 3] = n;
}

static void full(buf_t *o, uint8_t version, uint32_t flags)
{
    u8(o, version);
    u8(o, flags >> 16);
    u8(o, flags >> 8);
    u8(o, flags);
}

/* Adds @p n to the 32-bit number at @p at. */
static void add32(buf_t *o, size_t at, uint32_t n)
{
    if (o->bad) {
        return;
    }
    uint8_t *p = o->b + at;
    const uint32_t v = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]) + n;
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void matrix(buf_t *o)
{
    static const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++) {
        u32(o, m[i]);
    }
}

/* ---- sound: Opus, or AAC in recordings made before 0.68 -------------------------- */

typedef struct {
    bool opus;
    uint32_t rate;
    uint8_t channels;
    uint8_t asc[2]; /* AAC's AudioSpecificConfig: object type, rate index, channels */
} sound_t;

/* An ADTS frame's header: @p hlen its length, @p a what it holds. */
static bool adts_parse(const uint8_t *p, size_t n, size_t *hlen, sound_t *a)
{
    static const uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000, 7350};
    if (n < 7 || p[0] != 0xff || (p[1] & 0xf6) != 0xf0) {
        return false;
    }
    const uint8_t object = (uint8_t)((p[2] >> 6) + 1);
    const uint8_t rate_index = (p[2] >> 2) & 0x0f;
    const uint8_t channels = (uint8_t)(((p[2] & 1) << 2) | (p[3] >> 6));
    *hlen = (p[1] & 1) ? 7 : 9; /* 9 with a CRC */
    if (rate_index >= 13 || n <= *hlen) {
        return false;
    }
    a->opus = false;
    a->rate = rates[rate_index];
    a->channels = channels;
    a->asc[0] = (uint8_t)((object << 3) | (rate_index >> 1));
    a->asc[1] = (uint8_t)(((rate_index & 1) << 7) | (channels << 3));
    return true;
}

/* An Opus packet behind its control header (a 0x3ff prefix, flags, the size
 * as 255s and the rest, then the trims and an extension when flagged). */
static bool opus_parse(const uint8_t *p, size_t n, size_t *hlen, sound_t *a)
{
    if (n < 3 || p[0] != 0x7f || (p[1] & 0xe0) != 0xe0) {
        return false;
    }
    size_t at = 2, size = 0;
    for (;;) {
        if (at >= n) {
            return false;
        }
        const uint8_t v = p[at++];
        size += v;
        if (v != 255) {
            break;
        }
    }
    at += (p[1] & 0x10) ? 2 : 0; /* start trim */
    at += (p[1] & 0x08) ? 2 : 0; /* end trim */
    if (p[1] & 0x04) {           /* control extension */
        if (at >= n) {
            return false;
        }
        at += 1 + p[at];
    }
    if (size == 0 || at + size != n) {
        return false;
    }
    *hlen = at;
    a->opus = true;
    a->rate = OPUS_RATE;
    a->channels = OPUS_CHANNELS;
    a->asc[0] = a->asc[1] = 0;
    return true;
}

static bool sound_parse(const uint8_t *p, size_t n, size_t *hlen, sound_t *a)
{
    return opus_parse(p, n, hlen, a) || adts_parse(p, n, hlen, a);
}

/* Samples in one frame: what a frame lasts when the times do not say. */
static uint32_t frame_len(const sound_t *a)
{
    return a->opus ? OPUS_FRAMES : AAC_FRAMES;
}

static bool same_sound(const sound_t *a, const sound_t *b)
{
    return a->opus == b->opus && a->rate == b->rate && a->channels == b->channels &&
           a->asc[0] == b->asc[0] && a->asc[1] == b->asc[1];
}

/* ---- the conversion ------------------------------------------------------------ */

typedef struct {
    uint32_t *size;
    uint32_t *delta; /* duration of each sample, 90 kHz */
    uint8_t *sync;
    uint32_t *offset; /* into mdat's data: the tracks are interleaved */
    size_t count;
    size_t cap;
} samples_t;

static bool sample_add(samples_t *s, uint32_t size, bool sync, uint64_t offset)
{
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        uint32_t *a = realloc(s->size, cap * sizeof(uint32_t));
        if (!a) return false;
        s->size = a;
        uint32_t *d = realloc(s->delta, cap * sizeof(uint32_t));
        if (!d) return false;
        s->delta = d;
        uint8_t *y = realloc(s->sync, cap);
        if (!y) return false;
        s->sync = y;
        uint32_t *o = realloc(s->offset, cap * sizeof(uint32_t));
        if (!o) return false;
        s->offset = o;
        s->cap = cap;
    }
    s->offset[s->count] = (uint32_t)offset;
    s->size[s->count] = size;
    s->delta[s->count] = 0;
    s->sync[s->count] = sync;
    s->count++;
    return true;
}

/* Durations, run-length coded. */
static void stts_box(buf_t *o, const samples_t *sm)
{
    const size_t stts = box(o, "stts");
    full(o, 0, 0);
    const size_t count_at = o->len;
    u32(o, 0);
    uint32_t runs = 0;
    for (size_t i = 0; i < sm->count;) {
        size_t j = i;
        while (j < sm->count && sm->delta[j] == sm->delta[i]) {
            j++;
        }
        u32(o, (uint32_t)(j - i));
        u32(o, sm->delta[i]);
        runs++;
        i = j;
    }
    if (!o->bad) {
        o->b[count_at] = runs >> 24;
        o->b[count_at + 1] = runs >> 16;
        o->b[count_at + 2] = runs >> 8;
        o->b[count_at + 3] = runs;
    }
    box_end(o, stts);
}

/*
 * Sizes and where the samples are. Without sound they are one run, one chunk;
 * with it every sample is a chunk of its own. Returns where the offsets are,
 * for adding the header's length once it is known.
 */
static size_t chunks_box(buf_t *o, const samples_t *sm, bool interleaved)
{
    size_t b = box(o, "stsc");
    full(o, 0, 0);
    u32(o, 1);
    u32(o, 1);
    u32(o, interleaved ? 1 : (uint32_t)sm->count);
    u32(o, 1);
    box_end(o, b);

    b = box(o, "stsz");
    full(o, 0, 0);
    u32(o, 0);
    u32(o, (uint32_t)sm->count);
    for (size_t i = 0; i < sm->count; i++) {
        u32(o, sm->size[i]);
    }
    box_end(o, b);

    const size_t stco = box(o, "stco");
    full(o, 0, 0);
    const size_t n = interleaved ? sm->count : 1;
    u32(o, (uint32_t)n);
    const size_t at = o->len;
    for (size_t i = 0; i < n; i++) {
        u32(o, sm->offset[i]);
    }
    box_end(o, stco);
    return at;
}

/* Starts the track @p delay_ms into the movie: an empty edit, then all of it. */
static void delay_edit(buf_t *o, uint32_t delay_ms, uint32_t duration_ms)
{
    const size_t edts = box(o, "edts");
    const size_t elst = box(o, "elst");
    full(o, 0, 0);
    u32(o, 2);
    u32(o, delay_ms);
    u32(o, 0xffffffffu); /* media time -1: nothing plays */
    u32(o, 0x10000);
    u32(o, duration_ms);
    u32(o, 0);
    u32(o, 0x10000);
    box_end(o, elst);
    box_end(o, edts);
}

static size_t audio_trak(buf_t *o, const samples_t *am, const sound_t *snd, uint32_t delay_ms)
{
    uint64_t duration = 0; /* in samples */
    for (size_t i = 0; i < am->count; i++) {
        duration += am->delta[i];
    }
    const uint32_t duration_ms = (uint32_t)(duration * 1000 / snd->rate);
    const size_t trak = box(o, "trak");
    size_t b = box(o, "tkhd");
    full(o, 0, 3);
    u32(o, 0);
    u32(o, 0);
    u32(o, 2);
    u32(o, 0);
    u32(o, delay_ms + duration_ms);
    zeros(o, 8);
    u16(o, 0);
    u16(o, 1);      /* alternate group */
    u16(o, 0x100);  /* volume */
    u16(o, 0);
    matrix(o);
    u32(o, 0);
    u32(o, 0);
    box_end(o, b);
    if (delay_ms) {
        delay_edit(o, delay_ms, duration_ms);
    }

    const size_t mdia = box(o, "mdia");
    b = box(o, "mdhd");
    full(o, 0, 0);
    u32(o, 0);
    u32(o, 0);
    u32(o, snd->rate);
    u32(o, (uint32_t)duration);
    u16(o, 0x55c4);
    u16(o, 0);
    box_end(o, b);
    b = box(o, "hdlr");
    full(o, 0, 0);
    u32(o, 0);
    put(o, (const uint8_t *)"soun", 4);
    zeros(o, 12);
    put(o, (const uint8_t *)"ESP-KVM\0", 8);
    box_end(o, b);

    const size_t minf = box(o, "minf");
    b = box(o, "smhd");
    full(o, 0, 0);
    u32(o, 0);
    box_end(o, b);
    const size_t dinf = box(o, "dinf");
    const size_t dref = box(o, "dref");
    full(o, 0, 0);
    u32(o, 1);
    b = box(o, "url ");
    full(o, 0, 1);
    box_end(o, b);
    box_end(o, dref);
    box_end(o, dinf);

    const size_t stbl = box(o, "stbl");
    const size_t stsd = box(o, "stsd");
    full(o, 0, 0);
    u32(o, 1);
    const size_t entry = box(o, snd->opus ? "Opus" : "mp4a");
    zeros(o, 6);
    u16(o, 1);
    zeros(o, 8);
    u16(o, snd->channels);
    u16(o, 16);
    u32(o, 0);
    u32(o, snd->rate << 16);
    if (snd->opus) {
        b = box(o, "dOps"); /* not a full box: its own version byte */
        u8(o, 0);
        u8(o, snd->channels);
        u16(o, OPUS_PRE_SKIP);
        u32(o, snd->rate);
        u16(o, 0); /* output gain */
        u8(o, 0);  /* channel mapping family: mono or stereo */
        box_end(o, b);
    } else {
        b = box(o, "esds");
        full(o, 0, 0);
        u8(o, 0x03); /* ES descriptor */
        u8(o, 3 + 2 + 13 + 2 + 2 + 3);
        u16(o, 2);   /* ES id */
        u8(o, 0);
        u8(o, 0x04); /* decoder config */
        u8(o, 13 + 2 + 2);
        u8(o, 0x40); /* MPEG-4 audio */
        u8(o, 0x15); /* audio stream */
        u8(o, 0);    /* buffer size, 24 bits */
        u16(o, 0x1800);
        u32(o, 0);   /* bitrates: not given */
        u32(o, 0);
        u8(o, 0x05); /* decoder specific: the AudioSpecificConfig */
        u8(o, 2);
        put(o, snd->asc, 2);
        u8(o, 0x06); /* SL config */
        u8(o, 1);
        u8(o, 2);
        box_end(o, b);
    }
    box_end(o, entry);
    box_end(o, stsd);
    stts_box(o, am);
    const size_t offsets = chunks_box(o, am, true);
    box_end(o, stbl);
    box_end(o, minf);
    box_end(o, mdia);
    box_end(o, trak);
    return offsets;
}

bool ts_to_mp4(const mp4_io_t *io, uint32_t width, uint32_t height, const mp4_chapter_t *chapters,
               int chapter_count, char *err, size_t err_cap)
{
    samples_t sm = {0}, am = {0};
    sound_t snd = {0};
    int64_t audio_first = -1, audio_prev = -1;
    nal_scan_t params = {0};
    buf_t head = {0};
    ts_reader_t rd = {.io = io};
    bool ok = false;
    uint64_t data_bytes = 0;
    int64_t first_pts = -1, prev_pts = -1;

    /* Pass one: the sample table. */
    int got;
    while ((got = next_au(&rd)) == 1) {
        const uint8_t *au = rd.ready.b;
        const size_t au_len = rd.ready.len;
        const int64_t pts = rd.ready.pts;
        if (rd.ready.audio) {
            /* Sound from the first picture on; a change of format is left out. */
            size_t hlen;
            sound_t a;
            if (!sm.count || !sound_parse(au, au_len, &hlen, &a) ||
                (am.count && !same_sound(&a, &snd))) {
                continue;
            }
            snd = a;
            if (am.count) {
                /* Back in time where segments join: one frame's length. */
                const int64_t d = (pts - audio_prev) * snd.rate / TIMESCALE;
                am.delta[am.count - 1] = (d > 0 && d < (int64_t)snd.rate * 10) ? (uint32_t)d : frame_len(&snd);
            } else {
                audio_first = pts;
            }
            audio_prev = pts;
            if (!sample_add(&am, (uint32_t)(au_len - hlen), true, data_bytes)) {
                got = -1;
                break;
            }
            data_bytes += au_len - hlen;
            continue;
        }
        nal_scan_t s = {.sps = params.sps, .pps = params.pps,
                        .sps_len = params.sps_len, .pps_len = params.pps_len};
        each_nal(au, au_len, scan_nal, &s);
        params.sps = s.sps;
        params.sps_len = s.sps_len;
        params.pps = s.pps;
        params.pps_len = s.pps_len;
        if (!s.size) {
            continue;
        }
        if (!sm.count && !s.idr) {
            continue; /* a file has to start on a picture that decodes on its own */
        }
        if (first_pts < 0) {
            first_pts = pts;
        }
        if (sm.count) {
            /* Times go back where a clip joins two segments, each counted from
             * its own start: the frame there lasts as long as the one before. */
            const int64_t d = pts - prev_pts;
            const uint32_t before = sm.count > 1 ? sm.delta[sm.count - 2] : TIMESCALE / 25;
            sm.delta[sm.count - 1] = (d > 0 && d < TIMESCALE * 10) ? (uint32_t)d : before;
        }
        prev_pts = pts;
        if (!sample_add(&sm, (uint32_t)s.size, s.idr, data_bytes)) {
            got = -1;
            break;
        }
        data_bytes += s.size;
    }
    if (got < 0) {
        snprintf(err, err_cap, "out of memory reading the recording");
        goto out;
    }
    if (!sm.count || !params.sps || !params.pps) {
        snprintf(err, err_cap, "the recording has no complete H.264 picture");
        goto out;
    }
    sm.delta[sm.count - 1] = sm.count > 1 ? sm.delta[sm.count - 2] : TIMESCALE / 25;
    if (data_bytes + 8 > 0xffffffffull) {
        snprintf(err, err_cap, "too long for one MP4 here (over 4 GB)");
        goto out;
    }
    uint64_t duration = 0;
    for (size_t i = 0; i < sm.count; i++) {
        duration += sm.delta[i];
    }
    const bool sound = am.count > 0;
    uint32_t movie_ms = (uint32_t)(duration * 1000 / TIMESCALE);
    uint32_t delay_ms = 0;
    if (sound) {
        am.delta[am.count - 1] = frame_len(&snd);
        if (audio_first > first_pts) {
            delay_ms = (uint32_t)((audio_first - first_pts) / (TIMESCALE / 1000));
        }
        uint64_t audio = 0;
        for (size_t i = 0; i < am.count; i++) {
            audio += am.delta[i];
        }
        const uint32_t end_ms = delay_ms + (uint32_t)(audio * 1000 / snd.rate);
        if (end_ms > movie_ms) {
            movie_ms = end_ms;
        }
    }

    /* The index, written before the samples so the file plays as it arrives. */
    size_t b = box(&head, "ftyp");
    put(&head, (const uint8_t *)"isom", 4);
    u32(&head, 0x200);
    put(&head, (const uint8_t *)"isomiso2avc1mp41", 16);
    box_end(&head, b);

    const size_t moov = box(&head, "moov");
    b = box(&head, "mvhd");
    full(&head, 0, 0);
    u32(&head, 0);
    u32(&head, 0);
    u32(&head, 1000);
    u32(&head, movie_ms);
    u32(&head, 0x10000);
    u16(&head, 0x100);
    zeros(&head, 10);
    matrix(&head);
    zeros(&head, 24);
    u32(&head, sound ? 3 : 2); /* the next track id */
    box_end(&head, b);

    const size_t trak = box(&head, "trak");
    b = box(&head, "tkhd");
    full(&head, 0, 3);
    u32(&head, 0);
    u32(&head, 0);
    u32(&head, 1);
    u32(&head, 0);
    u32(&head, (uint32_t)(duration * 1000 / TIMESCALE));
    zeros(&head, 8);
    u16(&head, 0);
    u16(&head, 0);
    u16(&head, 0);
    u16(&head, 0);
    matrix(&head);
    u32(&head, width << 16);
    u32(&head, height << 16);
    box_end(&head, b);

    const size_t mdia = box(&head, "mdia");
    b = box(&head, "mdhd");
    full(&head, 0, 0);
    u32(&head, 0);
    u32(&head, 0);
    u32(&head, TIMESCALE);
    u32(&head, (uint32_t)duration);
    u16(&head, 0x55c4); /* "und" */
    u16(&head, 0);
    box_end(&head, b);
    b = box(&head, "hdlr");
    full(&head, 0, 0);
    u32(&head, 0);
    put(&head, (const uint8_t *)"vide", 4);
    zeros(&head, 12);
    put(&head, (const uint8_t *)"ESP-KVM\0", 8);
    box_end(&head, b);

    const size_t minf = box(&head, "minf");
    b = box(&head, "vmhd");
    full(&head, 0, 1);
    zeros(&head, 8);
    box_end(&head, b);
    const size_t dinf = box(&head, "dinf");
    const size_t dref = box(&head, "dref");
    full(&head, 0, 0);
    u32(&head, 1);
    b = box(&head, "url ");
    full(&head, 0, 1);
    box_end(&head, b);
    box_end(&head, dref);
    box_end(&head, dinf);

    const size_t stbl = box(&head, "stbl");
    const size_t stsd = box(&head, "stsd");
    full(&head, 0, 0);
    u32(&head, 1);
    const size_t avc1 = box(&head, "avc1");
    zeros(&head, 6);
    u16(&head, 1);
    zeros(&head, 16);
    u16(&head, (uint16_t)width);
    u16(&head, (uint16_t)height);
    u32(&head, 0x480000);
    u32(&head, 0x480000);
    u32(&head, 0);
    u16(&head, 1);
    zeros(&head, 32);
    u16(&head, 0x18);
    u16(&head, 0xffff);
    b = box(&head, "avcC");
    u8(&head, 1);
    u8(&head, params.sps[1]);
    u8(&head, params.sps[2]);
    u8(&head, params.sps[3]);
    u8(&head, 0xff); /* four-byte NAL lengths */
    u8(&head, 0xe1); /* one SPS */
    u16(&head, (uint16_t)params.sps_len);
    put(&head, params.sps, params.sps_len);
    u8(&head, 1);
    u16(&head, (uint16_t)params.pps_len);
    put(&head, params.pps, params.pps_len);
    box_end(&head, b);
    box_end(&head, avc1);
    box_end(&head, stsd);

    stts_box(&head, &sm);

    const size_t stss = box(&head, "stss");
    full(&head, 0, 0);
    uint32_t syncs = 0;
    for (size_t i = 0; i < sm.count; i++) {
        syncs += sm.sync[i];
    }
    u32(&head, syncs);
    for (size_t i = 0; i < sm.count; i++) {
        if (sm.sync[i]) {
            u32(&head, (uint32_t)(i + 1));
        }
    }
    box_end(&head, stss);

    const size_t offset_at = chunks_box(&head, &sm, sound);
    box_end(&head, stbl);
    box_end(&head, minf);
    box_end(&head, mdia);
    box_end(&head, trak);
    const size_t audio_offset_at = sound ? audio_trak(&head, &am, &snd, delay_ms) : 0;

    /* Chapters, the way Nero wrote them and VLC, mpv and ffmpeg read them. */
    if (chapter_count > 0 && chapters[0].at_us < (int64_t)(duration * 1000000 / TIMESCALE)) {
        const size_t udta = box(&head, "udta");
        b = box(&head, "chpl");
        full(&head, 1, 0);
        u32(&head, 0);
        /* Only chapters inside the video: a cut-off recording can be shorter
         * than the moments it was meant to hold. */
        const int64_t length_us = (int64_t)(duration * 1000000 / TIMESCALE);
        int n = 0;
        for (int i = 0; i < chapter_count && n < 255; i++) {
            n += chapters[i].at_us < length_us;
        }
        u8(&head, (uint8_t)n);
        for (int i = 0, kept = 0; i < chapter_count && kept < n; i++) {
            if (chapters[i].at_us >= length_us) {
                continue;
            }
            kept++;
            int64_t t = chapters[i].at_us;
            if (t < 0) {
                t = 0;
            }
            u64(&head, (uint64_t)t * 10); /* 100 ns units */
            const char *title = chapters[i].title ? chapters[i].title : "";
            size_t tl = strlen(title);
            if (tl > 255) {
                tl = 255;
            }
            u8(&head, (uint8_t)tl);
            put(&head, (const uint8_t *)title, tl);
        }
        box_end(&head, b);
        box_end(&head, udta);
    }
    box_end(&head, moov);

    if (head.bad) {
        snprintf(err, err_cap, "out of memory building the MP4 index");
        goto out;
    }
    /* The offsets were counted from the samples' start; the header comes first. */
    const uint32_t data_at = (uint32_t)(head.len + 8);
    for (size_t i = 0; i < (sound ? sm.count : 1); i++) {
        add32(&head, offset_at + i * 4, data_at);
    }
    for (size_t i = 0; i < am.count; i++) {
        add32(&head, audio_offset_at + i * 4, data_at);
    }

    const uint8_t mdat[8] = {(uint8_t)((data_bytes + 8) >> 24), (uint8_t)((data_bytes + 8) >> 16),
                             (uint8_t)((data_bytes + 8) >> 8), (uint8_t)(data_bytes + 8),
                             'm', 'd', 'a', 't'};
    if (!io->write(io->io, head.b, head.len) || !io->write(io->io, mdat, 8)) {
        snprintf(err, err_cap, "could not write the MP4");
        goto out;
    }

    /* Pass two: the samples, in the same order the table counted them. */
    if (!io->rewind(io->io)) {
        snprintf(err, err_cap, "could not read the recording again");
        goto out;
    }
    reader_free(&rd);
    rd = (ts_reader_t){.io = io};
    size_t written = 0, audio_written = 0;
    bool started = false;
    while ((got = next_au(&rd)) == 1 && (written < sm.count || audio_written < am.count)) {
        const uint8_t *au = rd.ready.b;
        const size_t au_len = rd.ready.len;
        if (rd.ready.audio) {
            size_t hlen;
            sound_t a;
            if (!started || audio_written >= am.count || !sound_parse(au, au_len, &hlen, &a) ||
                !same_sound(&a, &snd)) {
                continue;
            }
            if (au_len - hlen != am.size[audio_written] || !io->write(io->io, au + hlen, au_len - hlen)) {
                snprintf(err, err_cap, "could not write the MP4");
                goto out;
            }
            audio_written++;
            continue;
        }
        nal_scan_t probe = {.sps = params.sps, .pps = params.pps,
                            .sps_len = params.sps_len, .pps_len = params.pps_len};
        each_nal(au, au_len, scan_nal, &probe);
        if (!probe.size || (!started && !probe.idr) || written >= sm.count) {
            continue;
        }
        started = true;
        nal_scan_t w = {.io = io, .ok = true};
        each_nal(au, au_len, write_nal, &w);
        if (!w.ok) {
            snprintf(err, err_cap, "could not write the MP4");
            goto out;
        }
        written++;
    }
    if (written != sm.count || audio_written != am.count) {
        snprintf(err, err_cap, "the recording changed while it was converted");
        goto out;
    }
    ok = true;

out:
    reader_free(&rd);
    free(sm.size);
    free(sm.delta);
    free(sm.sync);
    free(sm.offset);
    free(am.size);
    free(am.delta);
    free(am.sync);
    free(am.offset);
    free(params.sps);
    free(params.pps);
    free(head.b);
    return ok;
}
