/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * MMC replies for the virtual optical drive. Each reply is built whole in a
 * scratch buffer and copied out cut to what the host asked for: the host says
 * how much it wants in the CDB, and a reply longer than that is normal.
 */
#include "mmc.h"

#include <string.h>

#define SENSE_NOT_READY 0x02
#define SENSE_ILLEGAL_REQUEST 0x05

/* Media event codes (MMC-6, GET EVENT STATUS NOTIFICATION). */
#define EV_NONE 0
#define EV_NEW_MEDIA 2
#define EV_MEDIA_REMOVAL 3

#define PROFILE_NONE 0x0000
#define PROFILE_CD_ROM 0x0008
#define PROFILE_DVD_ROM 0x0010

static void be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int32_t fail(mmc_sense_t *s, uint8_t key, uint8_t asc)
{
    s->key = key;
    s->asc = asc;
    s->ascq = 0;
    return -1;
}

static int32_t no_medium(mmc_sense_t *s)
{
    return fail(s, SENSE_NOT_READY, 0x3A);
}

static int32_t bad_field(mmc_sense_t *s)
{
    return fail(s, SENSE_ILLEGAL_REQUEST, 0x24); /* invalid field in CDB */
}

static int32_t copy_out(uint8_t *out, uint32_t cap, const uint8_t *r, uint32_t len)
{
    const uint32_t n = len < cap ? len : cap;
    memcpy(out, r, n);
    return (int32_t)n;
}

/* A block count as the 32-bit LBA the commands carry; images stay under 4 GB
   for now, but a lead-out must not wrap if one ever does not. */
static uint32_t lba32(uint64_t v)
{
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
}

static bool is_dvd(const mmc_media_t *m)
{
    return m->blocks > MMC_DVD_ABOVE_BLOCKS;
}

/* An address as MSF, in the four bytes a TOC descriptor has: 0, M, S, F. The
   first 150 frames (two seconds) are the lead-in's, so LBA 0 is 00:02:00. */
static void msf(uint8_t *p, uint32_t lba)
{
    /* M is one byte; past 255 minutes (a DVD over ~2.2 GB) hold at the top
       instead of wrapping to a small address. */
    const uint32_t top = 256u * 60u * 75u - 1u;
    const uint32_t f = lba < top - 150u ? lba + 150u : top;
    p[0] = 0;
    p[1] = (uint8_t)(f / (60u * 75u));
    p[2] = (uint8_t)((f / 75u) % 60u);
    p[3] = (uint8_t)(f % 75u);
}

static void address(uint8_t *p, uint32_t lba, bool use_msf)
{
    if (use_msf) {
        msf(p, lba);
    } else {
        be32(p, lba);
    }
}

/* READ TOC/PMA/ATIP (0x43): one data track, one session. */
static int32_t read_toc(const uint8_t *cdb, const mmc_media_t *m, uint8_t *out, uint32_t cap,
                        mmc_sense_t *s)
{
    if (!m->present) {
        return no_medium(s);
    }
    const bool use_msf = (cdb[1] & 0x02) != 0;
    uint8_t format = cdb[2] & 0x0F;
    if (format == 0) {
        /* Old hosts put the format in the top bits of the control byte. */
        format = (uint8_t)(cdb[9] >> 6);
    }
    const uint8_t track = cdb[6];
    const uint32_t leadout = lba32(m->blocks);
    uint8_t r[64];
    memset(r, 0, sizeof(r));
    uint32_t len;

    if (format == 0) {
        if (track > 1 && track != 0xAA) {
            return bad_field(s);
        }
        len = 4;
        if (track <= 1) {
            r[len + 1] = 0x14; /* ADR 1, control: data track */
            r[len + 2] = 1;
            address(&r[len + 4], 0, use_msf);
            len += 8;
        }
        r[len + 1] = 0x14;
        r[len + 2] = 0xAA; /* the lead-out */
        address(&r[len + 4], leadout, use_msf);
        len += 8;
        r[2] = 1; /* first track */
        r[3] = 1; /* last track */
    } else if (format == 1) {
        /* Session info: the first track of the last session. */
        len = 12;
        r[2] = 1;
        r[3] = 1;
        r[5] = 0x14;
        r[6] = 1;
        address(&r[8], 0, use_msf);
    } else if (format == 2) {
        /* The raw TOC: points A0, A1, A2 and the one track, always as MSF. */
        len = 4;
        const struct {
            uint8_t point;
            uint8_t pmin, psec, pframe;
        } rows[4] = {
            {0xA0, 1, 0x00, 0}, /* first track 1, disc type CD-ROM */
            {0xA1, 1, 0, 0},    /* last track 1 */
            {0xA2, 0, 0, 0},    /* lead-out, filled in below */
            {0x01, 0, 2, 0},    /* track 1 at 00:02:00 */
        };
        for (int i = 0; i < 4; i++) {
            uint8_t *d = &r[len];
            d[0] = 1;    /* session */
            d[1] = 0x14; /* ADR 1, data */
            d[3] = rows[i].point;
            d[8] = rows[i].pmin;
            d[9] = rows[i].psec;
            d[10] = rows[i].pframe;
            if (rows[i].point == 0xA2) {
                uint8_t t[4];
                msf(t, leadout);
                d[8] = t[1];
                d[9] = t[2];
                d[10] = t[3];
            }
            len += 11;
        }
        r[2] = 1; /* first session */
        r[3] = 1; /* last session */
    } else {
        return bad_field(s);
    }
    be16(r, len - 2);
    return copy_out(out, cap, r, len);
}

/* One feature descriptor for GET CONFIGURATION. */
typedef struct {
    uint16_t code;
    uint8_t version;
    bool current;
    uint8_t len;
    uint8_t data[8];
} feature_t;

/* GET CONFIGURATION (0x46): a read-only DVD-ROM/CD-ROM drive on USB. */
static int32_t get_configuration(const uint8_t *cdb, const mmc_media_t *m, uint8_t *out,
                                 uint32_t cap, mmc_sense_t *s)
{
    const uint8_t rt = cdb[1] & 0x03;
    const uint16_t start = (uint16_t)((cdb[2] << 8) | cdb[3]);
    if (rt == 3) {
        return bad_field(s);
    }
    const bool dvd = m->present && is_dvd(m);
    const bool cd = m->present && !dvd;
    const uint16_t profile = !m->present ? PROFILE_NONE : dvd ? PROFILE_DVD_ROM : PROFILE_CD_ROM;

    feature_t f[7];
    memset(f, 0, sizeof(f));
    /* Profile List: DVD-ROM then CD-ROM, the one in the drive marked current. */
    f[0] = (feature_t){.code = 0x0000, .version = 0, .current = true, .len = 8};
    be16(&f[0].data[0], PROFILE_DVD_ROM);
    f[0].data[2] = dvd ? 1 : 0;
    be16(&f[0].data[4], PROFILE_CD_ROM);
    f[0].data[6] = cd ? 1 : 0;
    /* Core: the physical interface is USB (8); DBE set, as MMC wants. */
    f[1] = (feature_t){.code = 0x0001, .version = 1, .current = true, .len = 8};
    be32(&f[1].data[0], 8);
    f[1].data[4] = 0x01;
    /* Morphing: media changes are reported through polled events. */
    f[2] = (feature_t){.code = 0x0002, .version = 1, .current = true, .len = 4};
    f[2].data[0] = 0x02;
    /* Removable Medium: a tray that can eject. */
    f[3] = (feature_t){.code = 0x0003, .version = 0, .current = true, .len = 4};
    f[3].data[0] = (1u << 5) | 0x08;
    /* Random Readable: 2048-byte blocks, read 1 (CD) or 16 (DVD) at a time. */
    f[4] = (feature_t){.code = 0x0010, .version = 0, .current = m->present, .len = 8};
    be32(&f[4].data[0], 2048);
    be16(&f[4].data[4], dvd ? 16 : 1);
    f[5] = (feature_t){.code = 0x001E, .version = 0, .current = cd, .len = 4}; /* CD Read */
    f[6] = (feature_t){.code = 0x001F, .version = 0, .current = dvd, .len = 4}; /* DVD Read */

    uint8_t r[8 + 7 * 12];
    memset(r, 0, sizeof(r));
    uint32_t len = 8;
    for (int i = 0; i < 7; i++) {
        const bool pick = rt == 2 ? f[i].code == start
                                  : f[i].code >= start && (rt == 0 || f[i].current);
        if (!pick) {
            continue;
        }
        uint8_t *d = &r[len];
        be16(d, f[i].code);
        /* Every one of these is persistent: it does not come and go with a disc. */
        d[2] = (uint8_t)((f[i].version << 2) | (f[i].code <= 0x0003 ? 0x02 : 0) |
                         (f[i].current ? 1 : 0));
        d[3] = f[i].len;
        memcpy(&d[4], f[i].data, f[i].len);
        len += 4u + f[i].len;
    }
    be32(r, len - 4);
    be16(&r[6], profile);
    return copy_out(out, cap, r, len);
}

/* Remember what the host was told, and turn a difference into one event. */
static void track_media(const mmc_media_t *m, mmc_events_t *ev)
{
    if (!ev->seen) {
        ev->seen = true;
        ev->seen_present = m->present;
        ev->seen_id = m->id;
        /* A drive that starts with a disc in it reports it once, the way a
           real one does after power-on. */
        ev->pending = m->present ? EV_NEW_MEDIA : EV_NONE;
        return;
    }
    if (m->present != ev->seen_present || (m->present && m->id != ev->seen_id)) {
        ev->pending = m->present ? EV_NEW_MEDIA : EV_MEDIA_REMOVAL;
        ev->seen_present = m->present;
        ev->seen_id = m->id;
    }
}

/* GET EVENT STATUS NOTIFICATION (0x4A), polled, media class only. */
static int32_t get_event_status(const uint8_t *cdb, const mmc_media_t *m, mmc_events_t *ev,
                                uint8_t *out, uint32_t cap, mmc_sense_t *s)
{
    if ((cdb[1] & 0x01) == 0) {
        return bad_field(s); /* asynchronous notification is not offered */
    }
    track_media(m, ev);
    uint8_t r[8];
    memset(r, 0, sizeof(r));
    r[3] = 0x10; /* supported: the media class */
    if ((cdb[4] & 0x10) == 0) {
        /* Nothing asked for that we have: no event available. */
        be16(r, 2);
        r[2] = 0x80;
        return copy_out(out, cap, r, 4);
    }
    be16(r, 6);
    r[2] = 4; /* media class */
    r[4] = ev->pending;
    r[5] = m->present ? 0x02 : 0x00; /* media present, tray closed */
    ev->pending = EV_NONE;
    return copy_out(out, cap, r, 8);
}

/* READ DISC INFORMATION (0x51): a finished disc, one session, one track. */
static int32_t read_disc_information(const uint8_t *cdb, const mmc_media_t *m, uint8_t *out,
                                     uint32_t cap, mmc_sense_t *s)
{
    if ((cdb[1] & 0x07) != 0) {
        return bad_field(s); /* only standard disc information */
    }
    if (!m->present) {
        return no_medium(s);
    }
    uint8_t r[34];
    memset(r, 0, sizeof(r));
    be16(r, 32);
    r[2] = 0x0E; /* last session complete, disc complete, not erasable */
    r[3] = 1;    /* first track */
    r[4] = 1;    /* sessions */
    r[5] = 1;    /* first track in the last session */
    r[6] = 1;    /* last track in the last session */
    r[8] = 0x00; /* disc type: CD-ROM (and what a DVD-ROM reports too) */
    memset(&r[16], 0xFF, 8); /* no lead-in or lead-out to add: it is finished */
    return copy_out(out, cap, r, sizeof(r));
}

/* READ TRACK INFORMATION (0x52): track 1, the whole image. */
static int32_t read_track_information(const uint8_t *cdb, const mmc_media_t *m, uint8_t *out,
                                      uint32_t cap, mmc_sense_t *s)
{
    if (!m->present) {
        return no_medium(s);
    }
    const uint8_t type = cdb[1] & 0x03;
    const uint32_t n = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
    if ((type == 1 && n != 1) || (type == 0 && n >= lba32(m->blocks)) || type > 1) {
        return bad_field(s);
    }
    uint8_t r[36];
    memset(r, 0, sizeof(r));
    be16(r, 34);
    r[2] = 1;    /* track 1 */
    r[3] = 1;    /* session 1 */
    r[5] = 0x04; /* track mode: data, not copyable twice */
    r[6] = 0x01; /* data mode 1 */
    be32(&r[8], 0);                  /* track start */
    be32(&r[24], lba32(m->blocks));  /* track size */
    return copy_out(out, cap, r, sizeof(r));
}

/* MODE SENSE(10) (0x5A): the capabilities page, and no block descriptor. */
static int32_t mode_sense10(const uint8_t *cdb, uint8_t *out, uint32_t cap, mmc_sense_t *s)
{
    const uint8_t page = cdb[2] & 0x3F;
    if (page != 0x2A && page != 0x3F) {
        return bad_field(s);
    }
    uint8_t r[8 + 22];
    memset(r, 0, sizeof(r));
    uint8_t *p = &r[8];
    p[0] = 0x2A; /* MM capabilities and mechanical status */
    p[1] = 20;
    p[2] = 0x08;               /* reads DVD-ROM (and CD-ROM, implied) */
    p[6] = (1u << 5) | 0x08;   /* tray loader, can eject */
    be16(&p[8], 0x2B7);        /* obsolete max read speed: 4x DVD in kB/s, harmless */
    be16(&p[12], 0x2B7);       /* obsolete current read speed */
    be16(r, sizeof(r) - 2);
    return copy_out(out, cap, r, sizeof(r));
}

/* MECHANISM STATUS (0xBD): no changer, idle, tray closed. */
static int32_t mechanism_status(uint8_t *out, uint32_t cap)
{
    uint8_t r[8];
    memset(r, 0, sizeof(r));
    return copy_out(out, cap, r, sizeof(r));
}

int32_t mmc_command(const uint8_t cdb[16], const mmc_media_t *m, mmc_events_t *ev,
                    uint8_t *out, uint32_t cap, mmc_sense_t *sense)
{
    switch (cdb[0]) {
    case 0x43:
        return read_toc(cdb, m, out, cap, sense);
    case 0x46:
        return get_configuration(cdb, m, out, cap, sense);
    case 0x4A:
        return get_event_status(cdb, m, ev, out, cap, sense);
    case 0x51:
        return read_disc_information(cdb, m, out, cap, sense);
    case 0x52:
        return read_track_information(cdb, m, out, cap, sense);
    case 0x5A:
        return mode_sense10(cdb, out, cap, sense);
    case 0xBB: /* SET CD SPEED: there is one speed, and it is fine */
        return 0;
    case 0xBD:
        return mechanism_status(out, cap);
    default:
        return MMC_NOT_MMC;
    }
}
