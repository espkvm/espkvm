/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The muxer's output parsed back: every packet well-formed, counters
 * continuous, the tables' CRCs right, and each frame coming out of its PES
 * byte for byte with the time it went in with. Then the same with sound: an
 * Opus stream in the table, and its packets between the pictures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts_mux.h"

static int failures;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static uint8_t *stream;
static size_t stream_len;

static void collect(void *user, const uint8_t *pkt)
{
    (void)user;
    stream = realloc(stream, stream_len + TS_PACKET);
    memcpy(stream + stream_len, pkt, TS_PACKET);
    stream_len += TS_PACKET;
}

static uint32_t crc32_mpeg(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
        }
    }
    return crc;
}

typedef struct {
    uint8_t *data;
    size_t len;
    uint64_t pts;
    uint64_t pcr;
    bool random_access;
} pes_t;

#define MAX_FRAMES 64
static pes_t got[MAX_FRAMES];
static int got_n;
static pes_t got_audio[MAX_FRAMES];
static int got_audio_n;
static bool table_has_audio;

static void parse(void)
{
    int cc[0x2000];
    for (int i = 0; i < 0x2000; i++) {
        cc[i] = -1;
    }
    int tables = 0;
    pes_t *cur = NULL, *cur_audio = NULL;
    for (size_t off = 0; off < stream_len; off += TS_PACKET) {
        const uint8_t *p = stream + off;
        CHECK(p[0] == 0x47, "sync byte at %zu", off);
        const bool pusi = p[1] & 0x40;
        const int pid = ((p[1] & 0x1f) << 8) | p[2];
        const int afc = (p[3] >> 4) & 3;
        const int c = p[3] & 0x0f;
        CHECK(afc == 1 || afc == 3, "adaptation control %d", afc);
        if (cc[pid] >= 0) {
            CHECK(c == ((cc[pid] + 1) & 0x0f), "continuity on pid %d: %d after %d", pid, c, cc[pid]);
        }
        cc[pid] = c;
        size_t at = 4;
        uint64_t pcr = 0;
        bool have_pcr = false, ra = false;
        if (afc == 3) {
            const size_t af = p[4];
            CHECK(5 + af <= TS_PACKET, "adaptation field too long");
            if (af > 0) {
                ra = p[5] & 0x40;
                if (p[5] & 0x10) {
                    have_pcr = true;
                    pcr = ((uint64_t)p[6] << 25) | ((uint64_t)p[7] << 17) | ((uint64_t)p[8] << 9) |
                          ((uint64_t)p[9] << 1) | (p[10] >> 7);
                }
            }
            at = 5 + af;
        }
        if (pid == 0x0000 || pid == 0x1000) {
            CHECK(pusi, "table without start");
            const size_t s = at + 1 + p[at];
            const size_t sl = ((p[s + 1] & 0x0f) << 8) | p[s + 2];
            CHECK(crc32_mpeg(p + s, 3 + sl) == 0, "table CRC on pid %d", pid);
            if (pid == 0x1000) {
                CHECK(p[s + 12] == 0x1b, "stream type %02x", p[s + 12]);
                static const uint8_t opus_es[] = {0x06, 0xe1, 0x01, 0xf0, 10,  0x05, 4,
                                                  'O',  'p',  'u',  's',  0x7f, 2,    0x80, 2};
                table_has_audio = sl == 33 && memcmp(p + s + 17, opus_es, sizeof(opus_es)) == 0;
            }
            tables++;
            continue;
        }
        if (pid == 0x0101) {
            if (pusi) {
                CHECK(got_audio_n < MAX_FRAMES, "too many audio frames");
                cur_audio = &got_audio[got_audio_n++];
                memset(cur_audio, 0, sizeof(*cur_audio));
                CHECK(!have_pcr, "audio with a PCR");
                const uint8_t *h = p + at;
                CHECK(h[0] == 0 && h[1] == 0 && h[2] == 1 && h[3] == 0xbd, "audio PES start code");
                CHECK((h[7] & 0xc0) == 0x80 && h[8] == 5, "audio PTS flags");
                cur_audio->pts = ((uint64_t)(h[9] & 0x0e) << 29) | ((uint64_t)h[10] << 22) |
                                 ((uint64_t)(h[11] & 0xfe) << 14) | ((uint64_t)h[12] << 7) | (h[13] >> 1);
                cur_audio->pcr = ((size_t)h[4] << 8) | h[5]; /* PES length, kept here */
                at += 14;
            }
            CHECK(cur_audio != NULL, "audio before any audio start");
            if (cur_audio) {
                const size_t n = TS_PACKET - at;
                cur_audio->data = realloc(cur_audio->data, cur_audio->len + n);
                memcpy(cur_audio->data + cur_audio->len, p + at, n);
                cur_audio->len += n;
            }
            continue;
        }
        CHECK(pid == 0x0100, "unexpected pid %d", pid);
        if (pusi) {
            CHECK(got_n < MAX_FRAMES, "too many frames");
            cur = &got[got_n++];
            memset(cur, 0, sizeof(*cur));
            CHECK(have_pcr, "frame start without PCR");
            cur->pcr = pcr;
            cur->random_access = ra;
            const uint8_t *h = p + at;
            CHECK(h[0] == 0 && h[1] == 0 && h[2] == 1 && h[3] == 0xe0, "PES start code");
            CHECK((h[7] & 0xc0) == 0x80 && h[8] == 5, "PTS flags");
            cur->pts = ((uint64_t)(h[9] & 0x0e) << 29) | ((uint64_t)h[10] << 22) |
                       ((uint64_t)(h[11] & 0xfe) << 14) | ((uint64_t)h[12] << 7) | (h[13] >> 1);
            at += 14;
        }
        CHECK(cur != NULL, "payload before any frame start");
        if (!cur) {
            continue;
        }
        const size_t n = TS_PACKET - at;
        cur->data = realloc(cur->data, cur->len + n);
        memcpy(cur->data + cur->len, p + at, n);
        cur->len += n;
    }
    CHECK(tables >= 2, "no tables");
    CHECK(stream_len % TS_PACKET == 0, "stream not whole packets");
}

int main(void)
{
    ts_mux_t m;
    ts_mux_init(&m, collect, NULL);

    /* Sizes that land on every boundary: tiny, one byte either side of a packet's
     * room, several packets, and something IDR-sized. */
    const size_t sizes[] = {1, 2, 150, 155, 156, 157, 162, 176, 183, 184, 185, 367, 368, 369, 1000, 65536, 250000};
    const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
    uint8_t *frames[32];
    for (int i = 0; i < n; i++) {
        frames[i] = malloc(sizes[i]);
        for (size_t k = 0; k < sizes[i]; k++) {
            frames[i][k] = (uint8_t)(k * 31 + i * 7);
        }
        const uint64_t pts = 90000ull * 3600 * 24 * 2 + (uint64_t)i * 3003; /* two days in */
        ts_mux_frame(&m, frames[i], sizes[i], pts, i % 5 == 0);
    }
    parse();

    CHECK(got_n == n, "frames out %d, in %d", got_n, n);
    static const uint8_t aud[6] = {0, 0, 0, 1, 0x09, 0xf0};
    for (int i = 0; i < got_n && i < n; i++) {
        const uint64_t pts = 90000ull * 3600 * 24 * 2 + (uint64_t)i * 3003;
        CHECK(got[i].pts == ((pts + 63000) & 0x1ffffffffull), "frame %d PTS", i);
        CHECK(got[i].pcr == (pts & 0x1ffffffffull), "frame %d PCR", i);
        CHECK(got[i].pcr <= got[i].pts, "frame %d PCR after PTS", i);
        CHECK(got[i].random_access == (i % 5 == 0), "frame %d random access flag", i);
        CHECK(got[i].len == sizes[i] + 6, "frame %d length %zu, want %zu", i, got[i].len, sizes[i] + 6);
        if (got[i].len == sizes[i] + 6) {
            CHECK(memcmp(got[i].data, aud, 6) == 0, "frame %d access unit delimiter", i);
            CHECK(memcmp(got[i].data + 6, frames[i], sizes[i]) == 0, "frame %d bytes", i);
        }
    }
    CHECK(!table_has_audio, "an audio stream in a recording without sound");
    const size_t video_packets = stream_len / TS_PACKET;

    /* With sound: an Opus packet after each picture, sizes over the same edges
     * and over the 255 steps of the size field in front of it. */
    stream_len = 0;
    got_n = 0;
    ts_mux_init(&m, collect, NULL);
    m.audio = true;
    const size_t asizes[] = {7, 169, 170, 171, 183, 184, 185, 254, 255, 360, 700, 1275};
    const int an = (int)(sizeof(asizes) / sizeof(asizes[0]));
    uint8_t *afr[16];
    for (int i = 0; i < an; i++) {
        ts_mux_frame(&m, frames[i], sizes[i], (uint64_t)i * 3000, i == 0);
        afr[i] = malloc(asizes[i]);
        for (size_t k = 0; k < asizes[i]; k++) {
            afr[i][k] = (uint8_t)(k * 13 + i);
        }
        ts_mux_opus(&m, afr[i], asizes[i], (uint64_t)i * 1800);
    }
    parse();
    CHECK(table_has_audio, "no audio stream in the table");
    CHECK(got_n == an, "pictures out %d, in %d", got_n, an);
    CHECK(got_audio_n == an, "audio frames out %d, in %d", got_audio_n, an);
    for (int i = 0; i < got_audio_n && i < an; i++) {
        const uint8_t *d = got_audio[i].data;
        const size_t hl = 2 + asizes[i] / 255 + 1; /* control header: prefix and size */
        CHECK(got_audio[i].pts == (uint64_t)i * 1800 + 63000, "audio %d PTS", i);
        CHECK(got_audio[i].pcr == asizes[i] + 8 + hl, "audio %d PES length", i);
        CHECK(got_audio[i].len == asizes[i] + hl, "audio %d length %zu, want %zu", i, got_audio[i].len,
              asizes[i] + hl);
        if (got_audio[i].len == asizes[i] + hl) {
            size_t size = 0;
            for (size_t k = 2; k < hl; k++) {
                size += d[k];
            }
            CHECK(d[0] == 0x7f && d[1] == 0xe0, "audio %d control header %02x %02x", i, d[0], d[1]);
            CHECK(size == asizes[i], "audio %d size field %zu", i, size);
            CHECK(memcmp(d + hl, afr[i], asizes[i]) == 0, "audio %d bytes", i);
        }
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ts_mux: %d frames, %zu packets, all well-formed; %d with sound\n", n, video_packets, an);
    return 0;
}
