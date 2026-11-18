/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 * The optical drive's MMC replies, checked byte by byte against MMC-6.
 */
#include "mmc.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);       \
            g_fail++;                                                \
        }                                                            \
    } while (0)

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

int main(void)
{
    uint8_t out[256];
    mmc_sense_t s;
    mmc_events_t ev;
    memset(&ev, 0, sizeof(ev));
    /* A 700 MB CD image: 358400 blocks of 2048 bytes. */
    const mmc_media_t cd = {.present = true, .blocks = 358400, .id = 1};
    const mmc_media_t dvd = {.present = true, .blocks = 2000000, .id = 2};
    const mmc_media_t none = {.present = false, .blocks = 0, .id = 0};

    /* READ TOC, format 0, LBA: track 1 at 0, lead-out at the block count. */
    uint8_t toc[16] = {0x43, 0, 0, 0, 0, 0, 0, 0, 0xFF};
    int32_t n = mmc_command(toc, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 20);
    CHECK(be16(out) == 18);
    CHECK(out[2] == 1 && out[3] == 1);
    CHECK(out[5] == 0x14 && out[6] == 1 && be32(&out[8]) == 0);
    CHECK(out[13] == 0x14 && out[14] == 0xAA && be32(&out[16]) == 358400);

    /* The same as MSF: LBA 0 is 00:02:00; 358400 + 150 frames is 79:40:50. */
    toc[1] = 0x02;
    n = mmc_command(toc, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 20);
    CHECK(out[9] == 0 && out[10] == 2 && out[11] == 0);
    CHECK(out[17] == 79 && out[18] == 40 && out[19] == 50);

    /* A 4.5 GB DVD is past 255 minutes: MSF holds at the top, LBA is exact. */
    const mmc_media_t big = {.present = true, .blocks = 2200000, .id = 3};
    n = mmc_command(toc, &big, &ev, out, sizeof(out), &s);
    CHECK(n == 20 && out[17] == 255 && out[18] == 59 && out[19] == 74);
    toc[1] = 0;
    n = mmc_command(toc, &big, &ev, out, sizeof(out), &s);
    CHECK(n == 20 && be32(&out[16]) == 2200000);

    /* Cut to what the host asked for, as the transport does. */
    n = mmc_command(toc, &cd, &ev, out, 4, &s);
    CHECK(n == 4);

    /* Session info (format 1), and the raw TOC (format 2) with A0/A1/A2 + track 1. */
    toc[2] = 1;
    n = mmc_command(toc, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 12 && be16(out) == 10 && out[6] == 1);
    toc[2] = 2;
    n = mmc_command(toc, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 48 && be16(out) == 46);
    CHECK(out[4 + 3] == 0xA0 && out[4 + 8] == 1);
    CHECK(out[26 + 3] == 0xA2 && out[26 + 8] == 79 && out[26 + 9] == 40 && out[26 + 10] == 50);
    CHECK(out[37 + 3] == 0x01 && out[37 + 9] == 2);
    /* The old SFF-8020 way of asking for format 1: the top bits of byte 9. */
    toc[2] = 0;
    toc[9] = 0x40;
    n = mmc_command(toc, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 12);
    toc[9] = 0;

    /* A track that is not there, a format we do not have, and no disc. */
    toc[6] = 2;
    CHECK(mmc_command(toc, &cd, &ev, out, sizeof(out), &s) == -1 && s.key == 5 && s.asc == 0x24);
    toc[6] = 0;
    toc[2] = 5;
    CHECK(mmc_command(toc, &cd, &ev, out, sizeof(out), &s) == -1 && s.asc == 0x24);
    toc[2] = 0;
    CHECK(mmc_command(toc, &none, &ev, out, sizeof(out), &s) == -1 && s.key == 2 && s.asc == 0x3A);

    /* GET CONFIGURATION: CD-ROM current for a small image, DVD-ROM for a big one. */
    uint8_t conf[16] = {0x46, 0, 0, 0, 0, 0, 0, 0x01, 0x00};
    n = mmc_command(conf, &cd, &ev, out, sizeof(out), &s);
    CHECK(n > 8 && be32(out) == (uint32_t)n - 4);
    CHECK(be16(&out[6]) == 0x0008);
    CHECK(be16(&out[8]) == 0x0000);                           /* profile list first */
    CHECK(be16(&out[12]) == 0x0010 && (out[14] & 1) == 0);    /* DVD-ROM, not current */
    CHECK(be16(&out[16]) == 0x0008 && (out[18] & 1) == 1);    /* CD-ROM, current */
    n = mmc_command(conf, &dvd, &ev, out, sizeof(out), &s);
    CHECK(be16(&out[6]) == 0x0010);
    n = mmc_command(conf, &none, &ev, out, sizeof(out), &s);
    CHECK(n > 8 && be16(&out[6]) == 0x0000);
    /* RT=2: exactly one feature, Random Readable, with its 2048-byte blocks. */
    conf[1] = 2;
    conf[3] = 0x10;
    n = mmc_command(conf, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 8 + 12 && be16(&out[8]) == 0x0010 && be32(&out[12]) == 2048);
    /* RT=1 with no disc: only the current ones, so no CD Read or DVD Read. */
    conf[1] = 1;
    conf[3] = 0x1E;
    n = mmc_command(conf, &none, &ev, out, sizeof(out), &s);
    CHECK(n == 8);

    /* Media events: new media once after start, then nothing, then a removal,
       then new media for a swapped image. */
    mmc_events_t e;
    memset(&e, 0, sizeof(e));
    uint8_t gesn[16] = {0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8};
    n = mmc_command(gesn, &cd, &e, out, sizeof(out), &s);
    CHECK(n == 8 && be16(out) == 6 && out[2] == 4 && out[3] == 0x10);
    CHECK(out[4] == 2 && out[5] == 0x02);
    n = mmc_command(gesn, &cd, &e, out, sizeof(out), &s);
    CHECK(out[4] == 0);
    n = mmc_command(gesn, &none, &e, out, sizeof(out), &s);
    CHECK(out[4] == 3 && out[5] == 0);
    n = mmc_command(gesn, &dvd, &e, out, sizeof(out), &s);
    CHECK(out[4] == 2);
    mmc_media_t cd2 = cd;
    cd2.id = 9; /* another image, same size */
    mmc_command(gesn, &cd, &e, out, sizeof(out), &s);
    n = mmc_command(gesn, &cd2, &e, out, sizeof(out), &s);
    CHECK(out[4] == 2);
    /* A class we do not have: "no event available". Asynchronous: refused. */
    gesn[4] = 0x02;
    n = mmc_command(gesn, &cd, &e, out, sizeof(out), &s);
    CHECK(n == 4 && (out[2] & 0x80));
    gesn[1] = 0;
    CHECK(mmc_command(gesn, &cd, &e, out, sizeof(out), &s) == -1);

    /* READ DISC INFORMATION: complete, one session, one track. */
    uint8_t rdi[16] = {0x51, 0, 0, 0, 0, 0, 0, 0, 34};
    n = mmc_command(rdi, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 34 && be16(out) == 32 && out[2] == 0x0E && out[3] == 1 && out[6] == 1);

    /* READ TRACK INFORMATION by track number, and by an LBA inside it. */
    uint8_t rti[16] = {0x52, 0x01, 0, 0, 0, 1, 0, 0, 36};
    n = mmc_command(rti, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 36 && out[2] == 1 && be32(&out[24]) == 358400);
    rti[5] = 2;
    CHECK(mmc_command(rti, &cd, &ev, out, sizeof(out), &s) == -1);

    /* MODE SENSE(10), the capabilities page: reads DVD, tray, eject. */
    uint8_t ms[16] = {0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 64};
    n = mmc_command(ms, &cd, &ev, out, sizeof(out), &s);
    CHECK(n == 30 && be16(out) == 28 && out[8] == 0x2A && out[9] == 20 && out[10] == 0x08);
    ms[2] = 0x01;
    CHECK(mmc_command(ms, &cd, &ev, out, sizeof(out), &s) == -1);

    /* SET CD SPEED is fine with no data; READ(10) is not ours. */
    uint8_t speed[16] = {0xBB};
    CHECK(mmc_command(speed, &cd, &ev, out, sizeof(out), &s) == 0);
    uint8_t read10[16] = {0x28};
    CHECK(mmc_command(read10, &cd, &ev, out, sizeof(out), &s) == MMC_NOT_MMC);

    if (g_fail) {
        printf("%d failed\n", g_fail);
        return 1;
    }
    printf("mmc: all passed\n");
    return 0;
}
