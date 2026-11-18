/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ts_mux.h"

#include <string.h>

#define PID_PAT 0x0000
#define PID_PMT 0x1000
#define PID_VIDEO 0x0100
#define PID_AUDIO 0x0101
#define STREAM_H264 0x1b
#define STREAM_PRIVATE_PES 0x06 /* Opus is one, told apart by its descriptors */

/* The decoder clock runs this far behind the picture times, so every frame
 * arrives before it is due - 700 ms is what most muxers use. */
#define PCR_LEAD_90K 63000u

/* MPEG-2 CRC-32: polynomial 0x04C11DB7, not reflected, no final xor. */
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

static void header(uint8_t *pkt, uint16_t pid, bool start, uint8_t cc)
{
    pkt[0] = 0x47;
    pkt[1] = (uint8_t)((start ? 0x40 : 0x00) | ((pid >> 8) & 0x1f));
    pkt[2] = (uint8_t)(pid & 0xff);
    pkt[3] = (uint8_t)(0x10 | (cc & 0x0f)); /* payload only */
}

/* A table section in one packet: pointer field, the section, its CRC, padding. */
static void psi(ts_mux_t *m, uint16_t pid, uint8_t *cc, const uint8_t *section, size_t len)
{
    uint8_t pkt[TS_PACKET];
    memset(pkt, 0xff, sizeof(pkt));
    header(pkt, pid, true, *cc);
    *cc = (uint8_t)((*cc + 1) & 0x0f);
    pkt[4] = 0; /* pointer field */
    memcpy(pkt + 5, section, len);
    const uint32_t crc = crc32_mpeg(section, len);
    pkt[5 + len] = (uint8_t)(crc >> 24);
    pkt[6 + len] = (uint8_t)(crc >> 16);
    pkt[7 + len] = (uint8_t)(crc >> 8);
    pkt[8 + len] = (uint8_t)crc;
    m->out(m->user, pkt);
}

static void tables(ts_mux_t *m)
{
    /* section_length counts from after itself up to and including the CRC. */
    const uint8_t pat[] = {
        0x00,                   /* table_id: program association */
        0xb0, 13,               /* syntax, reserved, section_length */
        0x00, 0x01,             /* transport_stream_id */
        0xc1,                   /* version 0, current */
        0x00, 0x00,             /* section, last section */
        0x00, 0x01,             /* program 1 */
        (uint8_t)(0xe0 | (PID_PMT >> 8)), (uint8_t)(PID_PMT & 0xff),
    };
    psi(m, PID_PAT, &m->cc_pat, pat, sizeof(pat));

    const uint8_t pmt[] = {
        0x02,                   /* table_id: program map */
        0xb0, m->audio ? 33 : 18,
        0x00, 0x01,             /* program 1 */
        0xc1,
        0x00, 0x00,
        (uint8_t)(0xe0 | (PID_VIDEO >> 8)), (uint8_t)(PID_VIDEO & 0xff), /* PCR PID */
        0xf0, 0x00,             /* program_info_length 0 */
        STREAM_H264,
        (uint8_t)(0xe0 | (PID_VIDEO >> 8)), (uint8_t)(PID_VIDEO & 0xff),
        0xf0, 0x00,             /* ES_info_length 0 */
        STREAM_PRIVATE_PES,     /* the rest is left out without sound */
        (uint8_t)(0xe0 | (PID_AUDIO >> 8)), (uint8_t)(PID_AUDIO & 0xff),
        0xf0, 10,               /* ES_info_length */
        0x05, 4, 'O', 'p', 'u', 's', /* registration descriptor */
        0x7f, 2, 0x80, 2,       /* extension descriptor: Opus, 2 channels */
    };
    psi(m, PID_PMT, &m->cc_pmt, pmt, m->audio ? sizeof(pmt) : sizeof(pmt) - 15);
}

void ts_mux_init(ts_mux_t *m, ts_mux_out_t out, void *user)
{
    memset(m, 0, sizeof(*m));
    m->out = out;
    m->user = user;
}

/*
 * A PES packet - @p lead, then @p data - cut into transport packets on @p pid.
 * The video's first packet carries the clock (and marks a keyframe); the last
 * of any is padded out to 188 bytes with adaptation-field stuffing.
 */
static void pes_out(ts_mux_t *m, uint16_t pid, uint8_t *cc, const uint8_t *lead, size_t ln,
                    const uint8_t *data, size_t len, uint64_t pcr, bool keyframe)
{
    const bool video = pid == PID_VIDEO;
    const size_t total = ln + len;
    size_t sent = 0;
    bool first = true;
    while (sent < total) {
        uint8_t pkt[TS_PACKET];
        size_t at = 4;
        const size_t left = total - sent;
        size_t af = 0; /* adaptation field length byte's value, 0 = none */
        bool has_af = false;
        if (first && video) {
            has_af = true;
            af = 1 + 6; /* flags + PCR */
        }
        size_t room = TS_PACKET - 4 - (has_af ? 1 + af : 0);
        if (left < room) {
            const size_t pad = room - left;
            if (!has_af) {
                has_af = true;
                /* One spare byte is the length byte alone (a zero-length field);
                 * more take a flags byte and then stuffing. */
                af = pad - 1;
            } else {
                af += pad;
            }
            room = left;
        }

        header(pkt, pid, first, *cc);
        *cc = (uint8_t)((*cc + 1) & 0x0f);
        if (has_af) {
            pkt[3] |= 0x20;
            pkt[at++] = (uint8_t)af;
            if (af > 0) {
                size_t used = 1;
                uint8_t flags = 0;
                if (first && video) {
                    flags |= 0x10; /* PCR */
                    if (keyframe) {
                        flags |= 0x40; /* random access */
                    }
                }
                pkt[at++] = flags;
                if (first && video) {
                    pkt[at++] = (uint8_t)(pcr >> 25);
                    pkt[at++] = (uint8_t)(pcr >> 17);
                    pkt[at++] = (uint8_t)(pcr >> 9);
                    pkt[at++] = (uint8_t)(pcr >> 1);
                    pkt[at++] = (uint8_t)(((pcr & 1) << 7) | 0x7e);
                    pkt[at++] = 0x00;
                    used += 6;
                }
                memset(pkt + at, 0xff, af - used);
                at += af - used;
            }
        }
        /* Payload: the PES lead first, then the frame. */
        size_t n = room;
        while (n) {
            if (sent < ln) {
                const size_t k = (ln - sent) < n ? (ln - sent) : n;
                memcpy(pkt + at, lead + sent, k);
                at += k;
                sent += k;
                n -= k;
            } else {
                memcpy(pkt + at, data + (sent - ln), n);
                at += n;
                sent += n;
                n = 0;
            }
        }
        m->out(m->user, pkt);
        first = false;
    }
}

void ts_mux_frame(ts_mux_t *m, const uint8_t *data, size_t len, uint64_t pts_90k, bool keyframe)
{
    if (keyframe) {
        tables(m);
    }
    const uint64_t pts = (pts_90k + PCR_LEAD_90K) & 0x1ffffffffull;
    const uint64_t pcr = pts_90k & 0x1ffffffffull;

    /* PES header, then an access unit delimiter: some players will not split
     * frames in a transport stream without one, and the encoder does not add it. */
    uint8_t lead[14 + 6];
    size_t ln = 0;
    lead[ln++] = 0x00;
    lead[ln++] = 0x00;
    lead[ln++] = 0x01;
    lead[ln++] = 0xe0; /* stream_id: video */
    lead[ln++] = 0x00; /* PES_packet_length 0: unbounded, allowed for video */
    lead[ln++] = 0x00;
    lead[ln++] = 0x80; /* marker bits */
    lead[ln++] = 0x80; /* PTS only - the encoder makes no B-frames */
    lead[ln++] = 5;    /* header data length */
    lead[ln++] = (uint8_t)(0x21 | ((pts >> 29) & 0x0e));
    lead[ln++] = (uint8_t)(pts >> 22);
    lead[ln++] = (uint8_t)(0x01 | ((pts >> 14) & 0xfe));
    lead[ln++] = (uint8_t)(pts >> 7);
    lead[ln++] = (uint8_t)(0x01 | ((pts << 1) & 0xfe));
    static const uint8_t aud[6] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0};
    memcpy(lead + ln, aud, sizeof(aud));
    ln += sizeof(aud);

    pes_out(m, PID_VIDEO, &m->cc_video, lead, ln, data, len, pcr, keyframe);
}

void ts_mux_opus(ts_mux_t *m, const uint8_t *packet, size_t len, uint64_t pts_90k)
{
    if (len == 0 || len > KVM_TS_OPUS_MAX) {
        return;
    }
    const uint64_t pts = (pts_90k + PCR_LEAD_90K) & 0x1ffffffffull;
    /* The packet is preceded by its control header: a 0x3ff prefix, no trim,
     * and its size as a run of 255s and the rest. */
    const size_t size_bytes = len / 255 + 1;
    const size_t pes_len = 3 + 5 + 2 + size_bytes + len;
    uint8_t lead[14 + 2 + KVM_TS_OPUS_SIZE_BYTES];
    size_t ln = 0;
    lead[ln++] = 0x00;
    lead[ln++] = 0x00;
    lead[ln++] = 0x01;
    lead[ln++] = 0xbd; /* stream_id: private stream 1 */
    lead[ln++] = (uint8_t)(pes_len >> 8);
    lead[ln++] = (uint8_t)pes_len;
    lead[ln++] = 0x80;
    lead[ln++] = 0x80; /* PTS only */
    lead[ln++] = 5;
    lead[ln++] = (uint8_t)(0x21 | ((pts >> 29) & 0x0e));
    lead[ln++] = (uint8_t)(pts >> 22);
    lead[ln++] = (uint8_t)(0x01 | ((pts >> 14) & 0xfe));
    lead[ln++] = (uint8_t)(pts >> 7);
    lead[ln++] = (uint8_t)(0x01 | ((pts << 1) & 0xfe));
    lead[ln++] = 0x7f;
    lead[ln++] = 0xe0;
    for (size_t left = len; ; left -= 255) {
        if (left < 255) {
            lead[ln++] = (uint8_t)left;
            break;
        }
        lead[ln++] = 0xff;
    }
    pes_out(m, PID_AUDIO, &m->cc_audio, lead, ln, packet, len, 0, false);
}
