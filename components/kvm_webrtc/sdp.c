/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See sdp.h. The offer comes from a signed-in browser, but it is still text
 * off the network: every copy is bounded and every number is range-checked.
 */
#include "sdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PT 8

typedef struct {
    int pt;
    bool h264;
    bool pmode1;
    char plid[SDP_PLID_MAX];
    bool nack, pli, fir, remb;
} pt_info_t;

static bool starts(const char *line, size_t len, const char *prefix)
{
    const size_t n = strlen(prefix);
    return len >= n && memcmp(line, prefix, n) == 0;
}

static bool contains(const char *line, size_t len, const char *needle)
{
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(line + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* Copy the rest of a line after @p skip into @p out, bounded. */
static void copy_rest(const char *line, size_t len, size_t skip, char *out, size_t cap)
{
    size_t n = len > skip ? len - skip : 0;
    while (n && (line[skip + n - 1] == ' ' || line[skip + n - 1] == '\t')) {
        n--;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, line + skip, n);
    out[n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* "sha-256 AB:CD:..." -> 32 bytes. */
static bool parse_fingerprint(const char *v, size_t len, uint8_t out[32])
{
    if (!starts(v, len, "sha-256 ")) {
        return false;
    }
    v += 8;
    len -= 8;
    for (int i = 0; i < 32; i++) {
        const size_t at = (size_t)i * 3;
        if (at + 2 > len) {
            return false;
        }
        const int hi = hexval(v[at]), lo = hexval(v[at + 1]);
        if (hi < 0 || lo < 0 || (i < 31 && (at + 2 >= len || v[at + 2] != ':'))) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

/* A payload type at the start of @p v, or -1. */
static int read_pt(const char *v, size_t len, size_t *used)
{
    int pt = 0;
    size_t i = 0;
    while (i < len && i < 3 && v[i] >= '0' && v[i] <= '9') {
        pt = pt * 10 + (v[i] - '0');
        i++;
    }
    if (i == 0 || pt > 127) {
        return -1;
    }
    *used = i;
    return pt;
}

static pt_info_t *find_pt(pt_info_t *pts, int n, int pt)
{
    for (int i = 0; i < n; i++) {
        if (pts[i].pt == pt) {
            return &pts[i];
        }
    }
    return NULL;
}

/* "profile-level-id=42e01f" in an fmtp parameter list. */
static void read_fmtp(pt_info_t *p, const char *v, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && v[j] != ';') {
            j++;
        }
        const char *kv = v + i;
        size_t kl = j - i;
        while (kl && *kv == ' ') {
            kv++;
            kl--;
        }
        if (starts(kv, kl, "packetization-mode=")) {
            p->pmode1 = kl == 20 && kv[19] == '1';
        } else if (starts(kv, kl, "profile-level-id=")) {
            copy_rest(kv, kl, 17, p->plid, sizeof(p->plid));
        }
        i = j + 1;
    }
}

static bool token(const char *t)
{
    if (!*t) {
        return false;
    }
    for (; *t; t++) {
        const char c = *t;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

static bool hex_token(const char *t)
{
    for (; *t; t++) {
        if (hexval(*t) < 0) {
            return false;
        }
    }
    return true;
}

int sdp_parse_offer(const char *sdp, sdp_offer_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pt = -1;
    pt_info_t pts[MAX_PT];
    int pt_n = 0;
    /* 0: session level, 1: the video m-line, 2: one refused, 3: the audio one,
       4: the data channel. */
    int section = 0;
    bool took_video = false;
    bool audio_opus = false;

    const char *p = sdp;
    while (*p) {
        const char *eol = strchr(p, '\n');
        const size_t raw = eol ? (size_t)(eol - p) : strlen(p);
        size_t len = raw;
        if (len && p[len - 1] == '\r') {
            len--;
        }
        const char *line = p;
        p += raw + (eol ? 1 : 0);

        if (starts(line, len, "m=")) {
            char kind = 0;
            if (!took_video && starts(line, len, "m=video ")) {
                took_video = true;
                section = 1;
                kind = 'v';
            } else if (!out->has_audio && starts(line, len, "m=audio ")) {
                out->has_audio = true;
                out->audio_first = !took_video;
                section = 3;
                kind = 'a';
            } else if (!out->has_data && starts(line, len, "m=application ") &&
                       contains(line, len, " webrtc-datachannel")) {
                out->has_data = true;
                out->sctp_port = 5000;
                section = 4;
                kind = 'd';
            } else {
                section = 2;
            }
            if (kind && out->m_count < 3) {
                out->order[out->m_count] = kind;
            }
            out->m_count++;
            continue;
        }
        if (section == 2) {
            continue;
        }
        if (section == 4) {
            if (starts(line, len, "a=mid:")) {
                copy_rest(line, len, 6, out->data_mid, sizeof(out->data_mid));
            } else if (starts(line, len, "a=sctp-port:")) {
                char num[8];
                copy_rest(line, len, 12, num, sizeof(num));
                const int port = atoi(num);
                if (port > 0 && port <= 65535) {
                    out->sctp_port = port;
                }
            }
            if (!starts(line, len, "a=ice-") && !starts(line, len, "a=fingerprint:")) {
                continue;
            }
        }
        if (section == 3) {
            if (starts(line, len, "a=mid:")) {
                copy_rest(line, len, 6, out->audio_mid, sizeof(out->audio_mid));
            } else if (!audio_opus && starts(line, len, "a=rtpmap:")) {
                size_t used;
                const int pt = read_pt(line + 9, len - 9, &used);
                const char *codec = line + 9 + used;
                const size_t cl = len - 9 - used;
                if (pt >= 0 &&
                    (starts(codec, cl, " opus/48000/2") || starts(codec, cl, " OPUS/48000/2"))) {
                    out->audio_pt = pt;
                    audio_opus = true;
                }
                continue;
            }
            /* ICE and DTLS lines in a bundled audio section are the same as the
               video's; they are read below like any other. */
            if (!starts(line, len, "a=ice-") && !starts(line, len, "a=fingerprint:")) {
                continue;
            }
        }
        /* ICE and DTLS may sit at session or media level; media wins. */
        if (starts(line, len, "a=ice-ufrag:")) {
            copy_rest(line, len, 12, out->ufrag, sizeof(out->ufrag));
        } else if (starts(line, len, "a=ice-pwd:")) {
            copy_rest(line, len, 10, out->pwd, sizeof(out->pwd));
        } else if (starts(line, len, "a=fingerprint:")) {
            /* Only SHA-256 is used; a sha-384 or sha-512 line beside it (aiortc
               sends all three) must not undo it. */
            uint8_t fp[32];
            if (parse_fingerprint(line + 14, len - 14, fp)) {
                memcpy(out->fingerprint, fp, sizeof(fp));
                out->has_fingerprint = true;
            }
        } else if (section != 1) {
            continue;
        } else if (starts(line, len, "a=mid:")) {
            copy_rest(line, len, 6, out->mid, sizeof(out->mid));
        } else if (starts(line, len, "a=extmap:")) {
            size_t used = 0;
            const int id = read_pt(line + 9, len - 9, &used);
            if (id < 1) {
                continue;
            }
            /* "a=extmap:<id>[/direction] <uri>": skip a direction if there is one. */
            const char *uri = memchr(line + 9 + used, ' ', len - 9 - used);
            static const char playout[] = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";
            if (id >= 1 && id <= 14 && uri &&
                (size_t)(line + len - (uri + 1)) == sizeof(playout) - 1 &&
                memcmp(uri + 1, playout, sizeof(playout) - 1) == 0) {
                out->playout_ext = id;
            }
        } else if (starts(line, len, "a=rtcp-rsize")) {
            out->rtcp_rsize = true;
        } else if (starts(line, len, "a=rtpmap:")) {
            size_t used;
            const int pt = read_pt(line + 9, len - 9, &used);
            if (pt < 0 || pt_n >= MAX_PT || find_pt(pts, pt_n, pt)) {
                continue;
            }
            pt_info_t *e = &pts[pt_n++];
            memset(e, 0, sizeof(*e));
            e->pt = pt;
            const char *codec = line + 9 + used;
            const size_t cl = len - 9 - used;
            e->h264 = starts(codec, cl, " H264/90000") || starts(codec, cl, " h264/90000");
        } else if (starts(line, len, "a=fmtp:")) {
            size_t used;
            pt_info_t *e = find_pt(pts, pt_n, read_pt(line + 7, len - 7, &used));
            if (e && 7 + used < len) {
                read_fmtp(e, line + 7 + used + 1, len - 7 - used - 1);
            }
        } else if (starts(line, len, "a=rtcp-fb:")) {
            size_t used;
            pt_info_t *e = find_pt(pts, pt_n, read_pt(line + 10, len - 10, &used));
            if (!e) {
                continue;
            }
            const char *fb = line + 10 + used;
            const size_t fl = len - 10 - used;
            if (fl == 5 && memcmp(fb, " nack", 5) == 0) {
                e->nack = true;
            } else if (starts(fb, fl, " nack pli")) {
                e->pli = true;
            } else if (starts(fb, fl, " ccm fir")) {
                e->fir = true;
            } else if (starts(fb, fl, " goog-remb")) {
                e->remb = true;
            }
        }
    }

    /* Constrained Baseline first (what the encoder writes), then Baseline. */
    const pt_info_t *best = NULL;
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (int i = 0; i < pt_n; i++) {
            const pt_info_t *e = &pts[i];
            if (!e->h264 || !e->pmode1 || strlen(e->plid) != 6) {
                continue;
            }
            const bool cb = strncmp(e->plid, "42e0", 4) == 0 || strncmp(e->plid, "42E0", 4) == 0;
            const bool b = strncmp(e->plid, "4200", 4) == 0;
            if ((pass == 0 && cb) || (pass == 1 && b)) {
                best = e;
                break;
            }
        }
    }
    /* Video, and audio if any, or the answer would have to decline the rest;
       this console never offers more. The mids go back into the answer, so
       they must be plain tokens. */
    if (!took_video || out->m_count != 1 + out->has_audio + out->has_data || !best || !out->ufrag[0] ||
        !out->pwd[0] || !out->has_fingerprint || !token(out->mid) || !hex_token(best->plid)) {
        return -1;
    }
    if (out->has_audio &&
        (!audio_opus || !token(out->audio_mid) || strcmp(out->audio_mid, out->mid) == 0)) {
        return -1;
    }
    if (out->has_data && (!token(out->data_mid) || strcmp(out->data_mid, out->mid) == 0 ||
                          (out->has_audio && strcmp(out->data_mid, out->audio_mid) == 0))) {
        return -1;
    }
    out->pt = best->pt;
    memcpy(out->plid, best->plid, sizeof(out->plid));
    out->fb_nack = best->nack;
    out->fb_pli = best->pli;
    out->fb_fir = best->fir;
    out->fb_remb = best->remb;
    return 0;
}

typedef struct {
    char *b;
    size_t cap, len;
    bool full;
} sb_t;

static void put(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(sb_t *s, const char *fmt, ...)
{
    if (s->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(s->b + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= s->cap - s->len) {
        s->full = true;
        return;
    }
    s->len += (size_t)n;
}

/* The lines every bundled m-line repeats: transport, ICE, DTLS. */
static void put_transport(sb_t *s, const sdp_answer_params_t *a, const char *mid)
{
    put(s, "c=IN IP4 0.0.0.0\r\n");
    put(s, "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 ", a->ufrag, a->pwd);
    for (int i = 0; i < 32; i++) {
        put(s, i < 31 ? "%02X:" : "%02X\r\n", a->fingerprint[i]);
    }
    put(s, "a=setup:passive\r\na=mid:%s\r\na=sendonly\r\na=rtcp-mux\r\n", mid);
}

static void put_candidates(sb_t *s, const sdp_answer_params_t *a)
{
    for (int i = 0; i < a->cand_count; i++) {
        const uint8_t *ip = (const uint8_t *)&a->cands[i].ip_be;
        /* Host candidates only; priority per RFC 8445 5.1.2 with a local
           preference that keeps them in the order given. */
        const unsigned prio = (126u << 24) | ((unsigned)(65535 - i) << 8) | 255u;
        put(s, "a=candidate:%d 1 udp %u %u.%u.%u.%u %u typ host\r\n", i + 1, prio, ip[0], ip[1],
            ip[2], ip[3], (unsigned)a->cands[i].port);
    }
    put(s, "a=end-of-candidates\r\n");
}

static void put_audio(sb_t *s, const sdp_offer_t *o, const sdp_answer_params_t *a)
{
    put(s, "m=audio 9 UDP/TLS/RTP/SAVPF %d\r\n", o->audio_pt);
    put_transport(s, a, o->audio_mid);
    put(s, "a=rtpmap:%d opus/48000/2\r\n", o->audio_pt);
    put(s, "a=fmtp:%d minptime=10;stereo=1;sprop-stereo=1\r\n", o->audio_pt);
    /* The same stream id as the video: the browser plays the two together. */
    put(s, "a=msid:espkvm sound\r\na=ssrc:%u cname:espkvm\r\n", (unsigned)a->audio_ssrc);
    put_candidates(s, a);
}

static void put_video(sb_t *s, const sdp_offer_t *o, const sdp_answer_params_t *a)
{
    put(s, "m=video 9 UDP/TLS/RTP/SAVPF %d\r\n", o->pt);
    put_transport(s, a, o->mid);
    if (o->rtcp_rsize) {
        put(s, "a=rtcp-rsize\r\n");
    }
    put(s, "a=rtpmap:%d H264/90000\r\n", o->pt);
    if (o->fb_nack) {
        put(s, "a=rtcp-fb:%d nack\r\n", o->pt);
    }
    if (o->fb_pli) {
        put(s, "a=rtcp-fb:%d nack pli\r\n", o->pt);
    }
    if (o->fb_fir) {
        put(s, "a=rtcp-fb:%d ccm fir\r\n", o->pt);
    }
    /* No goog-remb: Chrome's estimate starts near 300 kbit/s and climbs for
       tens of seconds, so following it would blur the first half minute. The
       bitrate follows loss instead, the same way it does for the WebSocket. */
    put(s,
        "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s\r\n",
        o->pt, o->plid);
    if (o->playout_ext) {
        put(s, "a=extmap:%d http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n",
            o->playout_ext);
    }
    put(s, "a=msid:espkvm screen\r\na=ssrc:%u cname:espkvm\r\n", (unsigned)a->ssrc);
    put_candidates(s, a);
}

/* The data channel: SCTP over the same DTLS. The device's own SCTP port is
   5000 too; nothing else shares the association. */
static void put_data(sb_t *s, const sdp_offer_t *o, const sdp_answer_params_t *a)
{
    put(s, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n");
    put(s, "c=IN IP4 0.0.0.0\r\n");
    put(s, "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 ", a->ufrag, a->pwd);
    for (int i = 0; i < 32; i++) {
        put(s, i < 31 ? "%02X:" : "%02X\r\n", a->fingerprint[i]);
    }
    put(s, "a=setup:passive\r\na=mid:%s\r\na=sctp-port:5000\r\na=max-message-size:1024\r\n",
        o->data_mid);
    put_candidates(s, a);
}

static const char *mid_of(const sdp_offer_t *o, char kind)
{
    return kind == 'a' ? o->audio_mid : kind == 'd' ? o->data_mid : o->mid;
}

int sdp_write_answer(const sdp_offer_t *o, const sdp_answer_params_t *a, char *out, size_t cap)
{
    if (cap == 0) {
        return -1;
    }
    sb_t s = {.b = out, .cap = cap};
    const int n = o->m_count < 3 ? o->m_count : 3;
    put(&s, "v=0\r\no=- %u 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n", (unsigned)a->ssrc);
    put(&s, "a=group:BUNDLE");
    for (int i = 0; i < n; i++) {
        put(&s, " %s", mid_of(o, o->order[i]));
    }
    put(&s, "\r\na=ice-lite\r\na=msid-semantic: WMS espkvm\r\n");
    for (int i = 0; i < n; i++) {
        if (o->order[i] == 'a') {
            put_audio(&s, o, a);
        } else if (o->order[i] == 'd') {
            put_data(&s, o, a);
        } else {
            put_video(&s, o, a);
        }
    }
    if (s.full) {
        return -1;
    }
    return (int)s.len;
}

