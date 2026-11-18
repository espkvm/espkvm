/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stream made by ts_mux, turned into an MP4, and the MP4 read back box by
 * box: index before data, every frame in the table at its length, the
 * keyframes marked, the chapters there. The real decoder check is done with
 * ffmpeg when the code changes; this keeps the structure honest in CI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts_mux.h"
#include "ts_to_mp4.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                            \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t *b;
    size_t len, cap, pos;
} mem_t;

static void mem_put(mem_t *m, const uint8_t *p, size_t n)
{
    if (m->len + n > m->cap) {
        m->cap = (m->len + n) * 2;
        m->b = realloc(m->b, m->cap);
    }
    memcpy(m->b + m->len, p, n);
    m->len += n;
}

static mem_t ts, mp4;
static void on_packet(void *u, const uint8_t *p) { (void)u; mem_put(&ts, p, TS_PACKET); }
static size_t rd(void *io, uint8_t *b, size_t n)
{
    mem_t *m = io;
    size_t k = m->len - m->pos < n ? m->len - m->pos : n;
    memcpy(b, m->b + m->pos, k);
    m->pos += k;
    return k;
}
static bool rw(void *io) { ((mem_t *)io)->pos = 0; return true; }
static bool wr(void *io, const uint8_t *d, size_t n) { (void)io; mem_put(&mp4, d, n); return true; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* The first box of @p type inside [p, p+n), searching into containers. */
static const uint8_t *find(const uint8_t *p, size_t n, const char *type, size_t *len)
{
    static const char *const containers[] = {"moov", "trak", "mdia", "minf", "stbl", "udta", "edts"};
    size_t i = 0;
    while (i + 8 <= n) {
        const uint32_t sz = be32(p + i);
        if (sz < 8 || i + sz > n) {
            return NULL;
        }
        if (!memcmp(p + i + 4, type, 4)) {
            *len = sz;
            return p + i;
        }
        for (size_t c = 0; c < 7; c++) {
            if (!memcmp(p + i + 4, containers[c], 4)) {
                const uint8_t *r = find(p + i + 8, sz - 8, type, len);
                if (r) {
                    return r;
                }
            }
        }
        i += sz;
    }
    return NULL;
}

/* An ADTS frame: AAC-LC, 48 kHz, stereo, @p payload bytes after the header. */
static size_t adts(uint8_t *out, size_t payload, uint8_t fill)
{
    const size_t len = 7 + payload;
    const uint8_t h[7] = {0xff, 0xf1, (1 << 6) | (3 << 2), (uint8_t)((2 << 6) | (len >> 11)),
                          (uint8_t)(len >> 3), (uint8_t)(((len & 7) << 5) | 0x1f), 0xfc};
    memcpy(out, h, 7);
    memset(out + 7, fill, payload);
    return len;
}

/*
 * An AAC frame the way recordings before 0.68 carry it: a PES of its own on
 * the audio PID, here always short enough for one transport packet.
 */
static void old_aac(const uint8_t *adts_frame, size_t len, uint64_t pts_90k)
{
    static uint8_t cc;
    const uint64_t pts = pts_90k + 63000;
    uint8_t pkt[TS_PACKET];
    const size_t pes_len = 3 + 5 + len;
    const size_t payload = 14 + len;
    const size_t af = TS_PACKET - 4 - payload; /* the stuffing, its length byte included */
    pkt[0] = 0x47;
    pkt[1] = 0x41;
    pkt[2] = 0x01;
    pkt[3] = (uint8_t)(0x30 | (cc++ & 0x0f));
    pkt[4] = (uint8_t)(af - 1);
    if (af > 1) {
        pkt[5] = 0;
        memset(pkt + 6, 0xff, af - 2);
    }
    uint8_t *h = pkt + 4 + af;
    const uint8_t lead[14] = {0, 0, 1, 0xc0, (uint8_t)(pes_len >> 8), (uint8_t)pes_len, 0x80, 0x80, 5,
                              (uint8_t)(0x21 | ((pts >> 29) & 0x0e)), (uint8_t)(pts >> 22),
                              (uint8_t)(0x01 | ((pts >> 14) & 0xfe)), (uint8_t)(pts >> 7),
                              (uint8_t)(0x01 | ((pts << 1) & 0xfe))};
    memcpy(h, lead, 14);
    memcpy(h + 14, adts_frame, len);
    mem_put(&ts, pkt, TS_PACKET);
}

/*
 * With sound, the way the recorder writes it: the sound frames up to a
 * picture's time go in before it, the first 10 ms after the first picture.
 * The MP4 gets a second track, the tracks interleaved, each sample where its
 * table says. Opus now; AAC as older recordings have it.
 */
static void sound(bool opus)
{
    ts.len = ts.pos = 0;
    mp4.len = 0;
    ts_mux_t m;
    ts_mux_init(&m, on_packet, NULL);
    m.audio = opus;
    static const uint8_t idr[] = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x28, 0xda, 0x01,
                                  0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80, 0, 0, 1, 0x65, 0x11, 0x22};
    static const uint8_t p_frame[] = {0, 0, 1, 0x41, 0x33, 0x44, 0x55};
    const uint64_t step = opus ? 1800 : 1920; /* 20 ms, or 1024 frames at 48 kHz */
    const size_t base = opus ? 200 : 100;
    uint64_t next_audio = 900; /* 10 ms */
    int audio_n = 0;
    for (int i = 0; i < 20; i++) {
        const uint64_t t = (uint64_t)i * 3600;
        while (next_audio <= t) {
            uint8_t f[400];
            if (opus) {
                memset(f, audio_n, base + (size_t)audio_n);
                ts_mux_opus(&m, f, base + (size_t)audio_n, next_audio);
            } else {
                old_aac(f, adts(f, base + (size_t)audio_n, (uint8_t)audio_n), next_audio);
            }
            audio_n++;
            next_audio += step;
        }
        if (i == 0) {
            ts_mux_frame(&m, idr, sizeof(idr), t, true);
        } else {
            ts_mux_frame(&m, p_frame, sizeof(p_frame), t, false);
        }
    }
    mp4_io_t io = {rd, rw, wr, &ts};
    char err[128] = "";
    CHECK(ts_to_mp4(&io, 1280, 720, NULL, 0, err, sizeof(err)));
    if (err[0]) {
        printf("  error: %s\n", err);
    }
    size_t len;
    const uint8_t *mvhd = find(mp4.b, mp4.len, "mvhd", &len);
    CHECK(mvhd && be32(mvhd + 96 + 8) == 3);
    const uint8_t *entry = NULL, *conf = NULL;
    for (size_t i = 4; i + 4 < mp4.len && !conf; i++) {
        if (!memcmp(mp4.b + i, opus ? "Opus" : "mp4a", 4)) entry = mp4.b + i - 4;
        if (!memcmp(mp4.b + i, opus ? "dOps" : "esds", 4)) conf = mp4.b + i - 4;
    }
    CHECK(entry && be32(entry + 32) == 48000u << 16 && entry[25] == 2);
    if (opus) {
        /* version 0, 2 channels, pre-skip 312, 48 kHz, gain 0, family 0 */
        static const uint8_t dops[] = {0, 2, 0x01, 0x38, 0, 0, 0xbb, 0x80, 0, 0, 0};
        CHECK(conf && be32(conf) == 8 + sizeof(dops) && !memcmp(conf + 8, dops, sizeof(dops)));
    } else {
        CHECK(conf && conf[12] == 0x03 && conf[13] == 25 && conf[32] == 0x05 && conf[34] == 0x11 &&
              conf[35] == 0x90);
    }
    /* The audio track: the second trak. */
    const uint8_t *t1 = find(mp4.b, mp4.len, "trak", &len);
    CHECK(t1 != NULL);
    if (!t1) {
        return;
    }
    const uint8_t *t2 = t1 + len;
    const size_t t2len = be32(t2);
    CHECK(!memcmp(t2 + 4, "trak", 4));
    const uint8_t *elst = find(t2 + 8, t2len - 8, "elst", &len);
    CHECK(elst && be32(elst + 12) == 2 && be32(elst + 16) == 10 && be32(elst + 20) == 0xffffffffu);
    const uint8_t *stts = find(t2 + 8, t2len - 8, "stts", &len);
    CHECK(stts && be32(stts + 12) == 1 && be32(stts + 16) == (uint32_t)audio_n &&
          be32(stts + 20) == (opus ? 960u : 1024u));
    const uint8_t *stco = find(t2 + 8, t2len - 8, "stco", &len);
    const uint8_t *stsz = find(t2 + 8, t2len - 8, "stsz", &len);
    CHECK(stco && stsz && be32(stco + 12) == (uint32_t)audio_n);
    for (int k = 0; stco && stsz && k < audio_n; k++) {
        const uint32_t at = be32(stco + 16 + 4 * k), size = be32(stsz + 20 + 4 * k);
        CHECK(size == base + (uint32_t)k && at + size <= mp4.len);
        CHECK(mp4.b[at] == (uint8_t)k && mp4.b[at + size - 1] == (uint8_t)k);
    }
    /* The pictures' chunks point at their slices. */
    const uint8_t *vco = find(t1 + 8, be32(t1) - 8, "stco", &len);
    CHECK(vco && be32(vco + 12) == 20);
    CHECK(vco && mp4.b[be32(vco + 16) + 4] == 0x65 && mp4.b[be32(vco + 20) + 4] == 0x41);
    const char *out = getenv(opus ? "MP4_OUT" : "MP4_OUT_AAC");
    FILE *f = fopen(out ? out : "/dev/null", "wb");
    if (f) {
        fwrite(mp4.b, 1, mp4.len, f);
        fclose(f);
    }
}

int main(void)
{
    ts_mux_t m;
    ts_mux_init(&m, on_packet, NULL);
    /* 50 frames at 25 fps; an IDR with SPS and PPS every 10. Sizes vary. */
    static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x28, 0xda, 0x01};
    static const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
    size_t expect_data = 0;
    for (int i = 0; i < 50; i++) {
        uint8_t au[4096];
        size_t n = 0;
        const bool idr = i % 10 == 0;
        if (idr) {
            memcpy(au + n, sps, sizeof(sps));
            n += sizeof(sps);
            memcpy(au + n, pps, sizeof(pps));
            n += sizeof(pps);
        }
        const size_t body = 100 + (size_t)i * 37;
        au[n++] = 0;
        au[n++] = 0;
        au[n++] = 1;
        au[n++] = idr ? 0x65 : 0x41;
        for (size_t k = 1; k < body; k++) {
            au[n++] = (uint8_t)(0x10 + k % 200); /* no accidental start codes */
        }
        expect_data += 4 + body; /* length prefix + the slice NAL */
        /* Two dashcam segments joined: the second starts its own muxer and
         * its own clock at frame 30, and the timing must not notice. */
        if (i == 30) {
            ts_mux_init(&m, on_packet, NULL);
        }
        ts_mux_frame(&m, au, n, (uint64_t)(i < 30 ? i : i - 30) * 3600, idr);
    }

    mp4_io_t io = {rd, rw, wr, &ts};
    const mp4_chapter_t ch[] = {{0, "Before"}, {1000000, "Event"}, {99000000, "After the end"}};
    char err[128] = "";
    CHECK(ts_to_mp4(&io, 1920, 1080, ch, 3, err, sizeof(err)));
    if (err[0]) {
        printf("  error: %s\n", err);
    }

    /* ftyp, moov, mdat - in that order. */
    CHECK(mp4.len > 16 && !memcmp(mp4.b + 4, "ftyp", 4));
    const size_t moov_at = be32(mp4.b);
    CHECK(!memcmp(mp4.b + moov_at + 4, "moov", 4));
    const size_t mdat_at = moov_at + be32(mp4.b + moov_at);
    CHECK(!memcmp(mp4.b + mdat_at + 4, "mdat", 4));
    CHECK(be32(mp4.b + mdat_at) == expect_data + 8);
    CHECK(mdat_at + 8 + expect_data == mp4.len);

    size_t len;
    const uint8_t *stsz = find(mp4.b, mp4.len, "stsz", &len);
    CHECK(stsz && be32(stsz + 16) == 50);
    const uint8_t *stss = find(mp4.b, mp4.len, "stss", &len);
    CHECK(stss && be32(stss + 12) == 5 && be32(stss + 16) == 1 && be32(stss + 20) == 11);
    const uint8_t *stco = find(mp4.b, mp4.len, "stco", &len);
    CHECK(stco && be32(stco + 16) == mdat_at + 8);
    const uint8_t *stts = find(mp4.b, mp4.len, "stts", &len);
    CHECK(stts && be32(stts + 12) == 1 && be32(stts + 16) == 50 && be32(stts + 20) == 3600);
    /* avcC sits inside stsd/avc1, behind their fixed fields: look for its tag. */
    const uint8_t *avcc = NULL;
    for (size_t i = 4; i + 4 < moov_at + be32(mp4.b + moov_at) && !avcc; i++) {
        if (!memcmp(mp4.b + i, "avcC", 4)) {
            avcc = mp4.b + i - 4;
        }
    }
    CHECK(avcc && avcc[9] == 0x42 && avcc[11] == 0x28);
    const uint8_t *chpl = find(mp4.b, mp4.len, "chpl", &len);
    CHECK(chpl && chpl[16] == 2); /* the chapter past the end is left out */
    /* The first sample is the IDR slice, length-prefixed, without SPS/PPS/AUD. */
    CHECK(be32(mp4.b + mdat_at + 8) == 100 && mp4.b[mdat_at + 12] == 0x65);

    sound(true);
    sound(false);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ts_to_mp4: index first, 50 samples, 5 keyframes, chapters, joined segments, sound - all as expected\n");
    return 0;
}
