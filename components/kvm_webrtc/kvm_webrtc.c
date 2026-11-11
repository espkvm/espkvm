/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_webrtc.h.
 *
 * Two tasks and one UDP socket. The receive task owns the socket and every
 * session's state changes: it answers STUN, runs the DTLS handshakes, reads
 * the browser's RTCP and closes sessions. The send task only turns frames into
 * SRTP packets for the sessions that are up. A session is ended by marking it,
 * and only the receive task frees it, so nothing is freed under a sender.
 *
 * One socket for every session, as ICE-lite allows: a STUN request names its
 * session in USERNAME, and after that the session is known by the address its
 * checks came from.
 */
#include "kvm_webrtc.h"

#include <errno.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

#include "capture.h"
#include "kvm_audio.h"
#include "kvm_caps.h"
#include "rtp_h264.h"
#include "sctp.h"
#include "sdp.h"
#include "sps_patch.h"
#include "srtp.h"
#include "stun.h"
#include "video_frame.h"
#include "wr_cert.h"

static const char *TAG = "webrtc";

#define MAX_SESSIONS 2
/* RTP payload per packet. With the RTP header, the playout-delay extension and
   the SRTP tag a datagram is 1230 bytes, which still fits inside a WireGuard
   tunnel's 1280-byte MTU. */
#define RTP_PAYLOAD 1200
#define PKT_CAP (RTP_HDR_LEN + RTP_EXT_LEN + RTP_PAYLOAD + SRTP_TAG_LEN)
/* Sent packets kept for retransmission on a NACK: about two keyframes. */
#define RING 256
#define DGRAM_MAX 1500

/* How long a session may take to connect, and how long a connected one may
   go without the browser's consent checks (it sends one every few seconds). */
#define CONNECT_TIMEOUT_US (20 * 1000000LL)
#define CONSENT_TIMEOUT_US (15 * 1000000LL)
#define OWNER_CHECK_US (5 * 1000000LL)
/* A keyframe asked for and not here yet is asked for again after this. Every
   frame asking would get several in a row, one per rate-limit window. */
#define KEY_REASK_US (1000000LL)
/* The receive task's longest sleep: a closed session is freed within this,
   and a DTLS retransmission timer is never later than this. */
#define TICK_MS 50
/* The longest a data channel packet waits for its acknowledgement. */
#define SACK_DELAY_US (50 * 1000LL)

/* With a datagram and a data channel record on it, next to mbedTLS. */
#define RX_STACK (12 * 1024)
#define TX_STACK 4096
#define AU_STACK 3072
#define RX_PRIO 5
#define TX_PRIO 5
/* Level with the video; the Opus encoder feeding it sits below both. */
#define AU_PRIO 5
/* Opus packets waiting for the send task: 200 ms. */
#define AU_DEPTH 10

typedef enum {
    ST_FREE = 0,
    ST_CLAIMED, /* taken by an offer being answered; no packet can match it */
    ST_ICE,   /* waiting for the browser's connectivity checks */
    ST_DTLS,  /* handshake under way */
    ST_MEDIA, /* sending */
    ST_DEAD,  /* marked for the receive task to free */
} state_t;

typedef struct {
    state_t st;
    uint32_t id;
    uint8_t owner[32];
    char ufrag[9];
    char pwd[33];
    wr_hmac_t *ice;
    uint8_t remote_fp[32];
    bool fp_ok; /* the browser's certificate matched remote_fp */

    struct sockaddr_in peer;
    bool have_peer;
    int64_t created_us, seen_us, owner_checked_us;

    mbedtls_ssl_context ssl;
    bool ssl_on;
    const uint8_t *in; /* the datagram the DTLS bio hands over next */
    size_t in_len;
    int64_t t_int_us, t_fin_us; /* DTLS timer; t_fin_us 0 when stopped */
    /* The handshake's master secret and randoms, for the SRTP keys; wiped
       once they are made. */
    uint8_t master[48];
    uint8_t randoms[64]; /* client random, then server random */
    mbedtls_tls_prf_types prf;
    bool have_master;

    srtp_ctx_t tx, rx;
    bool srtp_on;
    rtp_h264_t rtp;
    bool need_key;
    int64_t key_asked_us; /* when this session last asked for a keyframe */
    uint8_t *pkt;  /* PKT_CAP, the packet being built */
    uint8_t *ring; /* RING * PKT_CAP */
    uint16_t ring_seq[RING];
    uint16_t ring_len[RING];

    /* The Opus stream, when the offer asked for one. Its own SSRC, sequence
       and SRTP context (same keys, separate rollover counter). */
    bool audio;
    bool audio_on; /* the console turned the sound on */
    uint8_t audio_pt;
    uint32_t audio_ssrc;
    uint16_t audio_seq;
    uint32_t audio_ts0; /* random start, as RFC 3550 asks */
    srtp_ctx_t atx;

    /* The data channel, when the offer had one: SCTP in DTLS records, only
       ever received (see sctp.h). */
    bool data;
    sctp_assoc_t sctp;
    int64_t sack_us; /* when the last SCTP answer went out */

    uint32_t frames, pkts, resent, plis, drops, audio_pkts, inputs;
} sess_t;

static sess_t s_sess[MAX_SESSIONS];
static SemaphoreHandle_t s_mu;
/* Given whenever a session reaches ST_MEDIA, so the send task can sleep while
   there is nobody to send to. */
static SemaphoreHandle_t s_media;
/* Given by the receive task each time it frees a session. */
static SemaphoreHandle_t s_freed;
static int s_sock = -1;
static uint16_t s_port;
static wr_cert_t s_cert;
static mbedtls_ssl_config s_conf;
static volatile bool s_ready;
static kvm_webrtc_owner_alive_fn s_alive;
static kvm_webrtc_input_fn s_input;
static uint32_t s_next_id = 1;

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void random_token(char *out, size_t n)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < n; i++) {
        out[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    out[n] = 0;
}

/* ---- DTLS plumbing ---------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    sess_t *s = ctx;
    const int r = sendto(s_sock, buf, len, 0, (const struct sockaddr *)&s->peer, sizeof(s->peer));
    if (r < 0) {
        return (errno == EAGAIN || errno == ENOMEM) ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                     : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    sess_t *s = ctx;
    if (!s->in_len) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    /* A datagram is read whole or not at all: DTLS records do not span them. */
    if (s->in_len > len) {
        s->in_len = 0;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    const size_t n = s->in_len;
    memcpy(buf, s->in, n);
    s->in_len = 0;
    return (int)n;
}

/* mbedTLS's retransmission timer, on esp_timer (MBEDTLS_TIMING_C is off). */
static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    sess_t *s = ctx;
    const int64_t now = esp_timer_get_time();
    s->t_int_us = now + (int64_t)int_ms * 1000;
    s->t_fin_us = fin_ms ? now + (int64_t)fin_ms * 1000 : 0;
}

static int timer_get(void *ctx)
{
    sess_t *s = ctx;
    if (!s->t_fin_us) {
        return -1;
    }
    const int64_t now = esp_timer_get_time();
    return now >= s->t_fin_us ? 2 : now >= s->t_int_us ? 1 : 0;
}

/* ---- session lifetime --------------------------------------------------- */

static void session_free(sess_t *s)
{
    if (s->ssl_on) {
        if (s->srtp_on && s->have_peer) {
            (void)mbedtls_ssl_close_notify(&s->ssl); /* best effort */
        }
        mbedtls_ssl_free(&s->ssl);
    }
    if (s->srtp_on) {
        srtp_free(&s->tx);
        srtp_free(&s->rx);
        if (s->audio) {
            srtp_free(&s->atx);
        }
    }
    wr_hmac_free(s->ice);
    heap_caps_free(s->ring);
    heap_caps_free(s->pkt);
    ESP_LOGI(TAG,
             "session %u closed: %u frames, %u packets, %u resent, %u PLIs, %u dropped, "
             "%u audio packets, %u inputs",
             (unsigned)s->id, (unsigned)s->frames, (unsigned)s->pkts, (unsigned)s->resent,
             (unsigned)s->plis, (unsigned)s->drops, (unsigned)s->audio_pkts, (unsigned)s->inputs);
    memset(s, 0, sizeof(*s));
    video_frame_viewer_leave();
}

static void session_kill(sess_t *s, const char *why)
{
    if (s->st != ST_FREE && s->st != ST_DEAD) {
        ESP_LOGI(TAG, "session %u: %s", (unsigned)s->id, why);
        s->st = ST_DEAD;
    }
}

/*
 * The browser's certificate is self-signed and trusted by the fingerprint in
 * its offer alone. Checked here, during the handshake: this build does not
 * keep the peer's certificate after it (MBEDTLS_SSL_KEEP_PEER_CERTIFICATE off),
 * and a wrong one then ends the handshake at once.
 */
static int verify_peer(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    sess_t *s = ctx;
    if (depth != 0) {
        *flags = 0;
        return 0;
    }
    uint8_t fp[32];
    size_t hl = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, crt->raw.p, crt->raw.len, fp, sizeof(fp), &hl) !=
            PSA_SUCCESS ||
        hl != 32 || memcmp(fp, s->remote_fp, 32) != 0) {
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    s->fp_ok = true;
    *flags = 0;
    return 0;
}

/*
 * The SRTP keys come from the master secret (RFC 5764 4.2). Taken here and not
 * with mbedtls_ssl_export_keying_material(): a DTLS server keeps its new keys
 * aside until the browser's first record after the handshake, and that call
 * reads them from where they are not yet (a NULL dereference in mbedTLS 4).
 */
static void export_keys(void *ctx, mbedtls_ssl_key_export_type type, const unsigned char *secret,
                        size_t secret_len, const unsigned char client_random[32],
                        const unsigned char server_random[32], mbedtls_tls_prf_types prf)
{
    sess_t *s = ctx;
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || secret_len != sizeof(s->master)) {
        return;
    }
    memcpy(s->master, secret, sizeof(s->master));
    memcpy(s->randoms, client_random, 32);
    memcpy(s->randoms + 32, server_random, 32);
    s->prf = prf;
    s->have_master = true;
}

static void dtls_start(sess_t *s)
{
    mbedtls_ssl_init(&s->ssl);
    s->ssl_on = true;
    if (mbedtls_ssl_setup(&s->ssl, &s_conf) != 0) {
        session_kill(s, "DTLS setup failed");
        return;
    }
    mbedtls_ssl_set_bio(&s->ssl, s, bio_send, bio_recv, NULL);
    mbedtls_ssl_set_timer_cb(&s->ssl, s, timer_set, timer_get);
    mbedtls_ssl_set_verify(&s->ssl, verify_peer, s);
    mbedtls_ssl_set_export_keys_cb(&s->ssl, export_keys, s);
    mbedtls_ssl_set_mtu(&s->ssl, RTP_HDR_LEN + RTP_PAYLOAD);
    s->st = ST_DTLS;
}

/* The handshake is done: check it was the browser that offered, take the SRTP
   keys out of it, and start sending. */
static void dtls_done(sess_t *s)
{
    if (!s->fp_ok) {
        session_kill(s, "the browser sent no certificate");
        return;
    }
    mbedtls_dtls_srtp_info srtp;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&s->ssl, &srtp);
    if (srtp.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80) {
        session_kill(s, "no SRTP profile in common");
        return;
    }
    /* RFC 5764 4.2: client key, server key, client salt, server salt. */
    uint8_t km[2 * (SRTP_KEY_LEN + SRTP_SALT_LEN)];
    const int kr = s->have_master ? mbedtls_ssl_tls_prf(s->prf, s->master, sizeof(s->master),
                                                        "EXTRACTOR-dtls_srtp", s->randoms,
                                                        sizeof(s->randoms), km, sizeof(km))
                                  : -1;
    memset(s->master, 0, sizeof(s->master));
    if (kr != 0) {
        session_kill(s, "could not derive the SRTP keys");
        return;
    }
    const uint8_t *ck = km, *sk = km + SRTP_KEY_LEN;
    const uint8_t *cs = km + 2 * SRTP_KEY_LEN, *ss = cs + SRTP_SALT_LEN;
    /* The device is the DTLS server: it sends with the server's keys. */
    const int r1 = srtp_init(&s->tx, sk, ss);
    const int r2 = srtp_init(&s->rx, ck, cs);
    const int r3 = s->audio ? srtp_init(&s->atx, sk, ss) : 0;
    memset(km, 0, sizeof(km));
    if (r1 || r2 || r3) {
        if (!r1) {
            srtp_free(&s->tx);
        }
        if (!r2) {
            srtp_free(&s->rx);
        }
        if (s->audio && !r3) {
            srtp_free(&s->atx);
        }
        session_kill(s, "SRTP setup failed");
        return;
    }
    s->srtp_on = true;
    s->need_key = true;
    s->key_asked_us = esp_timer_get_time();
    s->st = ST_MEDIA;
    char ip[16];
    inet_ntoa_r(s->peer.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "session %u: sending to %s:%u", (unsigned)s->id, ip, ntohs(s->peer.sin_port));
    video_frame_request_keyframe();
    xSemaphoreGive(s_media);
}

static void on_channel(void *ctx, uint16_t stream, uint32_t ppid, const uint8_t *data, size_t len)
{
    sess_t *s = ctx;
    const kvm_webrtc_input_fn fn = s_input;
    if (stream == 0 && ppid == SCTP_PPID_BINARY && fn) {
        s->inputs++;
        fn(s->owner, data, len);
    }
}

static void dtls_step(sess_t *s)
{
    if (s->st == ST_DTLS) {
        const int r = mbedtls_ssl_handshake(&s->ssl);
        if (r == 0) {
            dtls_done(s);
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGW(TAG, "session %u: DTLS handshake failed (-0x%04x)", (unsigned)s->id, -r);
            session_kill(s, "DTLS failed");
        }
        return;
    }
    if (s->st == ST_MEDIA) {
        /* Over DTLS once SRTP runs: the data channel's SCTP, one packet a
           record, or an alert. */
        uint8_t rec[DGRAM_MAX], reply[256];
        for (;;) {
            const int r = mbedtls_ssl_read(&s->ssl, rec, sizeof(rec));
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                session_kill(s, "the browser closed it");
                return;
            }
            if (r <= 0) {
                return;
            }
            if (!s->data) {
                continue;
            }
            const size_t n = sctp_input(&s->sctp, rec, (size_t)r, reply, sizeof(reply), on_channel, s);
            if (n > 0) {
                (void)mbedtls_ssl_write(&s->ssl, reply, n);
                s->sack_us = esp_timer_get_time();
            }
        }
    }
}

/* ---- incoming packets --------------------------------------------------- */

static void resend(sess_t *s, uint16_t seq)
{
    const int slot = seq % RING;
    if (s->ring_len[slot] && s->ring_seq[slot] == seq) {
        (void)sendto(s_sock, s->ring + (size_t)slot * PKT_CAP, s->ring_len[slot], 0,
                     (const struct sockaddr *)&s->peer, sizeof(s->peer));
        s->resent++;
    }
}

static void on_rtcp(sess_t *s, const uint8_t *p, size_t len)
{
    size_t off = 0;
    while (off + 4 <= len) {
        const uint8_t fmt = p[off] & 0x1f;
        const uint8_t pt = p[off + 1];
        const size_t plen = ((size_t)(p[off + 2] << 8 | p[off + 3]) + 1) * 4;
        if ((p[off] >> 6) != 2 || off + plen > len) {
            return;
        }
        const uint8_t *b = p + off;
        if (pt == 201) {
            /*
             * Receiver report, about once a second: the share of packets lost
             * since the last one, out of 256. The encoder steps its bitrate
             * down at two misses in a second, so a link losing 5% or more
             * counts twice and cuts it on its own; 2% counts once and cuts it
             * together with any other sign of a slow link. Only the video's
             * block counts; the audio is too small to say much.
             */
            for (size_t r = 0; r < fmt && 8 + 24 * (r + 1) <= plen; r++) {
                const uint8_t *rb = b + 8 + 24 * r;
                const uint32_t ssrc = (uint32_t)rb[0] << 24 | (uint32_t)rb[1] << 16 |
                                      (uint32_t)rb[2] << 8 | rb[3];
                if (ssrc != s->rtp.ssrc) {
                    continue;
                }
                const uint8_t lost = rb[4];
                if (lost >= 13) {
                    capture_link_miss();
                    capture_link_miss();
                } else if (lost >= 5) {
                    capture_link_miss();
                }
            }
        } else if (pt == 205 && fmt == 1) {
            /* Generic NACK: a lost packet and a bitmap of the 16 after it. */
            for (size_t i = 12; i + 4 <= plen; i += 4) {
                const uint16_t pid = (uint16_t)(b[i] << 8 | b[i + 1]);
                const uint16_t blp = (uint16_t)(b[i + 2] << 8 | b[i + 3]);
                resend(s, pid);
                for (int k = 0; k < 16; k++) {
                    if (blp & (1u << k)) {
                        resend(s, (uint16_t)(pid + k + 1));
                    }
                }
            }
        } else if (pt == 206 && (fmt == 1 || fmt == 4)) {
            /* PLI or FIR: the decoder lost its reference. */
            s->plis++;
            video_frame_request_repair_keyframe();
        }
        off += plen;
    }
}

static sess_t *by_ufrag(const char *ufrag)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].st != ST_FREE && s_sess[i].st != ST_DEAD &&
            strcmp(s_sess[i].ufrag, ufrag) == 0) {
            return &s_sess[i];
        }
    }
    return NULL;
}

static sess_t *by_peer(const struct sockaddr_in *from)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if ((s_sess[i].st == ST_DTLS || s_sess[i].st == ST_MEDIA) && s_sess[i].have_peer &&
            same_addr(&s_sess[i].peer, from)) {
            return &s_sess[i];
        }
    }
    return NULL;
}

static void on_stun(const uint8_t *p, size_t len, const struct sockaddr_in *from)
{
    char ufrag[sizeof(s_sess[0].ufrag)];
    if (stun_local_ufrag(p, len, ufrag, sizeof(ufrag)) != 0) {
        return;
    }
    sess_t *s = by_ufrag(ufrag);
    stun_req_t req;
    if (!s || stun_check_binding(p, len, s->ufrag, s->ice, &req) != 0) {
        return; /* not ours, or not signed with our password: no answer at all */
    }
    uint8_t resp[STUN_RESPONSE_LEN];
    const size_t n = stun_build_success(req.txid, from->sin_addr.s_addr, from->sin_port, s->ice,
                                        resp, sizeof(resp));
    if (n) {
        (void)sendto(s_sock, resp, n, 0, (const struct sockaddr *)from, sizeof(*from));
    }
    s->seen_us = esp_timer_get_time();
    /* The pair the browser nominates is where media goes; until it nominates,
       the first address that passed a check is as good as any. */
    if (!s->have_peer || req.use_candidate) {
        s->peer = *from;
        s->have_peer = true;
    }
    if (s->st == ST_ICE) {
        dtls_start(s);
    }
}

static void on_datagram(uint8_t *p, size_t len, const struct sockaddr_in *from)
{
    /* RFC 7983: the first byte tells STUN, DTLS and RTP/RTCP apart. */
    if (stun_is(p, len)) {
        on_stun(p, len, from);
        return;
    }
    sess_t *s = by_peer(from);
    if (!s) {
        return;
    }
    if (p[0] >= 20 && p[0] <= 63) {
        s->in = p;
        s->in_len = len;
        dtls_step(s);
        s->in_len = 0;
    } else if (p[0] >= 128 && p[0] <= 191 && s->st == ST_MEDIA && len >= 2 && p[1] >= 192 &&
               p[1] <= 223) {
        const int n = srtcp_unprotect(&s->rx, p, len);
        if (n > 0) {
            s->seen_us = esp_timer_get_time();
            on_rtcp(s, p, (size_t)n);
        }
    }
}

/* ---- the receive task ----------------------------------------------------- */

static void housekeeping(void)
{
    const int64_t now = esp_timer_get_time();
    /* Switched off, or the encoder moved to MJPEG, which WebRTC cannot carry:
       the console sees the codec change and goes back to the WebSocket. */
    const char *gone = !kvm_cap_active(KVM_CAP_WEBRTC)               ? "WebRTC was switched off"
                       : video_frame_payload() == VIDEO_PAYLOAD_JPEG ? "the device moved to MJPEG"
                                                                     : NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        sess_t *s = &s_sess[i];
        if (s->st == ST_FREE || s->st == ST_CLAIMED) {
            continue;
        }
        if (gone) {
            session_kill(s, gone);
        }
        if (s->st == ST_DEAD) {
            session_free(s);
            xSemaphoreGive(s_freed);
            continue;
        }
        if (s->st != ST_MEDIA && now - s->created_us > CONNECT_TIMEOUT_US) {
            session_kill(s, "the browser never connected");
        } else if (s->st == ST_MEDIA && now - s->seen_us > CONSENT_TIMEOUT_US) {
            session_kill(s, "the browser stopped answering");
        } else if (now - s->owner_checked_us > OWNER_CHECK_US) {
            s->owner_checked_us = now;
            if (s_alive && !s_alive(s->owner)) {
                session_kill(s, "its sign-in ended");
            }
        }
        if (s->st == ST_DTLS && timer_get(s) == 2) {
            dtls_step(s); /* retransmit, or give up */
        }
        /* The data channel's acknowledgements, held back so a stream of
           moves does not cost a packet back for each (see sctp.h). */
        if (s->st == ST_MEDIA && s->data && s->sctp.unacked && now - s->sack_us >= SACK_DELAY_US) {
            uint8_t sack[64];
            const size_t n = sctp_sack(&s->sctp, sack, sizeof(sack));
            if (n > 0) {
                (void)mbedtls_ssl_write(&s->ssl, sack, n);
            }
            s->sack_us = now;
        }
    }
}

static bool conf_build(void)
{
    mbedtls_ssl_config_init(&s_conf);
    if (mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&s_conf, &s_cert.crt, &s_cert.key) != 0) {
        return false;
    }
    /* The browser's certificate is self-signed and trusted by its fingerprint
       alone, which verify_peer() checks: ask for it, but do not chain it. */
    mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    /* No HelloVerifyRequest: only an address that passed an ICE check with
       this session's password ever reaches DTLS. */
    mbedtls_ssl_conf_dtls_cookies(&s_conf, NULL, NULL, NULL);
    static const mbedtls_ssl_srtp_profile profiles[] = {MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
                                                        MBEDTLS_TLS_SRTP_UNSET};
    return mbedtls_ssl_conf_dtls_srtp_protection_profiles(&s_conf, profiles) == 0;
}

static void rx_task(void *arg)
{
    (void)arg;
    if (wr_cert_make(&s_cert) != 0 || !conf_build()) {
        ESP_LOGE(TAG, "no DTLS certificate: WebRTC is off");
        kvm_cap_report(KVM_CAP_WEBRTC, false, "could not make its DTLS certificate");
        vTaskDelete(NULL);
        return;
    }
    uint8_t *buf = heap_caps_malloc(DGRAM_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the receive buffer: WebRTC is off");
        kvm_cap_report(KVM_CAP_WEBRTC, false, "out of memory");
        vTaskDelete(NULL);
        return;
    }
    s_ready = true;
    kvm_cap_report(KVM_CAP_WEBRTC, true, NULL);
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = TICK_MS * 1000};
        if (select(s_sock + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(s_sock, &rfds)) {
            /* Drain what is waiting before sleeping again. */
            for (;;) {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                const int n = recvfrom(s_sock, buf, DGRAM_MAX, MSG_DONTWAIT,
                                       (struct sockaddr *)&from, &fl);
                if (n <= 0) {
                    break;
                }
                lock();
                on_datagram(buf, (size_t)n, &from);
                unlock();
            }
        }
        lock();
        housekeeping();
        unlock();
    }
}

/* ---- the send task -------------------------------------------------------- */

static int emit(void *ctx, uint8_t *pkt, size_t len)
{
    sess_t *s = ctx;
    const int n = srtp_protect(&s->tx, pkt, len, PKT_CAP);
    if (n < 0) {
        return -1;
    }
    const uint16_t seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
    const int slot = seq % RING;
    memcpy(s->ring + (size_t)slot * PKT_CAP, pkt, (size_t)n);
    s->ring_seq[slot] = seq;
    s->ring_len[slot] = (uint16_t)n;
    /*
     * lwIP has no blocking send for UDP: a full queue is ENOMEM at once. One
     * tick lets the driver drain a keyframe's burst; still full after that is
     * a link that cannot keep up, and the frame is given up.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (sendto(s_sock, pkt, (size_t)n, 0, (const struct sockaddr *)&s->peer,
                   sizeof(s->peer)) == n) {
            s->pkts++;
            return 0;
        }
        if (errno != ENOMEM && errno != EAGAIN) {
            break;
        }
        vTaskDelay(1);
    }
    return -1;
}

static bool any_media(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].st == ST_MEDIA) {
            return true;
        }
    }
    return false;
}

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t *frame = NULL;
    size_t frame_cap = 0;
    uint32_t last_seq = 0;
    for (;;) {
        lock();
        const bool busy = any_media();
        unlock();
        if (!busy) {
            xSemaphoreTake(s_media, pdMS_TO_TICKS(1000));
            last_seq = video_frame_seq();
            continue;
        }
        if (!video_frame_wait_new(last_seq, 500)) {
            continue;
        }
        video_frame_ref_t f;
        if (!video_frame_acquire(&f)) {
            vTaskDelay(1); /* a codec change set the slot aside; one tick, not 0 */
            last_seq = video_frame_seq();
            continue;
        }
        const bool gap = last_seq != 0 && (uint32_t)(f.seq - last_seq) > 1u;
        last_seq = f.seq;
        if (f.payload != VIDEO_PAYLOAD_H264) {
            video_frame_release(&f);
            continue;
        }
        /*
         * Held until it is sent. The encoder then has no free slot and waits,
         * so a slow link lowers the frame rate instead of skipping frames: a
         * skipped delta costs a keyframe, which takes longer still to send, and
         * the next one is skipped too. Only a keyframe is copied, for the SPS
         * patch; room for the few bytes the SPS grows by.
         */
        const uint8_t *au = f.data;
        size_t len = f.len;
        if (f.keyframe) {
            if (frame_cap < f.len + 16) {
                uint8_t *grown = heap_caps_realloc(frame, f.len + 16, MALLOC_CAP_SPIRAM);
                if (grown) {
                    frame = grown;
                    frame_cap = f.len + 16;
                }
            }
            const size_t patched =
                frame_cap >= f.len + 16 ? sps_patch_au(f.data, f.len, frame, frame_cap) : 0;
            if (patched) {
                au = frame;
                len = patched;
            }
        }
        const bool key = f.keyframe;
        /* 90 kHz from when the picture landed, so the browser paces it right. */
        const uint32_t ts = (uint32_t)((f.src_us ? f.src_us : f.at_us) * 9 / 100);

        bool repair = false;
        const int64_t now = esp_timer_get_time();
        lock();
        for (int i = 0; i < MAX_SESSIONS; i++) {
            sess_t *s = &s_sess[i];
            if (s->st != ST_MEDIA) {
                continue;
            }
            if (gap && !s->need_key) {
                s->need_key = true; /* a skipped delta breaks the chain */
                s->key_asked_us = 0;
            }
            if (s->need_key && !key) {
                if (now - s->key_asked_us > KEY_REASK_US) {
                    s->key_asked_us = now;
                    repair = true;
                }
                continue;
            }
            if (rtp_h264_send(&s->rtp, au, len, ts, s->pkt, emit, s) < 0) {
                s->drops++;
                s->need_key = true;
                s->key_asked_us = now;
                repair = true;
                capture_link_miss();
                continue;
            }
            s->need_key = false;
            s->frames++;
        }
        unlock();
        video_frame_release(&f);
        if (repair) {
            video_frame_request_repair_keyframe();
        }
    }
}

/* ---- the audio send task ------------------------------------------------- */

static bool any_audio(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].st == ST_MEDIA && s_sess[i].audio && s_sess[i].audio_on) {
            return true;
        }
    }
    return false;
}

/* One Opus packet per RTP packet (RFC 7587); no retransmission, a lost 20 ms
   is better skipped than played late. */
static void au_task(void *arg)
{
    (void)arg;
    kvm_opus_packet_t *in = heap_caps_malloc(sizeof(*in), MALLOC_CAP_SPIRAM);
    uint8_t *pkt = heap_caps_malloc(RTP_HDR_LEN + KVM_OPUS_MAX + SRTP_TAG_LEN, MALLOC_CAP_SPIRAM);
    const size_t cap = RTP_HDR_LEN + KVM_OPUS_MAX + SRTP_TAG_LEN;
    kvm_opus_reader_t *r = NULL;
    for (;;) {
        lock();
        const bool busy = any_audio();
        unlock();
        if (!busy || !in || !pkt) {
            kvm_opus_close(r);
            r = NULL;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!r && kvm_opus_open(AU_DEPTH, &r) != ESP_OK) {
            r = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000)); /* audio is off */
            continue;
        }
        if (kvm_opus_read(r, in, 100) != ESP_OK) {
            continue;
        }
        lock();
        for (int i = 0; i < MAX_SESSIONS; i++) {
            sess_t *s = &s_sess[i];
            if (s->st != ST_MEDIA || !s->audio || !s->audio_on) {
                continue;
            }
            const uint32_t ts = s->audio_ts0 + in->ts;
            pkt[0] = 0x80;
            pkt[1] = (uint8_t)((in->resync ? 0x80 : 0) | s->audio_pt);
            pkt[2] = (uint8_t)(s->audio_seq >> 8);
            pkt[3] = (uint8_t)s->audio_seq;
            pkt[4] = (uint8_t)(ts >> 24);
            pkt[5] = (uint8_t)(ts >> 16);
            pkt[6] = (uint8_t)(ts >> 8);
            pkt[7] = (uint8_t)ts;
            pkt[8] = (uint8_t)(s->audio_ssrc >> 24);
            pkt[9] = (uint8_t)(s->audio_ssrc >> 16);
            pkt[10] = (uint8_t)(s->audio_ssrc >> 8);
            pkt[11] = (uint8_t)s->audio_ssrc;
            memcpy(pkt + RTP_HDR_LEN, in->data, in->len);
            s->audio_seq++;
            const int n = srtp_protect(&s->atx, pkt, RTP_HDR_LEN + in->len, cap);
            if (n > 0 && sendto(s_sock, pkt, (size_t)n, 0, (const struct sockaddr *)&s->peer,
                                sizeof(s->peer)) == n) {
                s->audio_pkts++;
            }
        }
        unlock();
    }
}

/* ---- public ---------------------------------------------------------------- */

esp_err_t kvm_webrtc_init(kvm_webrtc_owner_alive_fn alive)
{
    s_alive = alive;
    s_mu = xSemaphoreCreateMutex();
    s_media = xSemaphoreCreateBinary();
    s_freed = xSemaphoreCreateCounting(MAX_SESSIONS, 0);
    if (!s_mu || !s_media || !s_freed) {
        return ESP_ERR_NO_MEM;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        kvm_cap_report(KVM_CAP_WEBRTC, false, "no socket free for it");
        return ESP_FAIL;
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t al = sizeof(a);
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(s_sock, (struct sockaddr *)&a, &al) != 0) {
        close(s_sock);
        s_sock = -1;
        kvm_cap_report(KVM_CAP_WEBRTC, false, "could not open its UDP port");
        return ESP_FAIL;
    }
    s_port = ntohs(a.sin_port);
    if (xTaskCreateWithCaps(rx_task, "webrtc_rx", RX_STACK, NULL, RX_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS ||
        xTaskCreateWithCaps(tx_task, "webrtc_tx", TX_STACK, NULL, TX_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS ||
        xTaskCreateWithCaps(au_task, "webrtc_au", AU_STACK, NULL, AU_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        kvm_cap_report(KVM_CAP_WEBRTC, false, "could not start its tasks");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening on UDP %u", (unsigned)s_port);
    return ESP_OK;
}

/* Every IPv4 address the device has right now: Ethernet, Wi-Fi, the setup
   hotspot, a WireGuard or Tailscale tunnel. The browser tries them all, in the
   order of their priority, which is the order here: a tunnel ("wg", both VPNs)
   last, or a console on the same LAN would take the slower path through it. */
static int candidates(sdp_candidate_t *out, int max)
{
    int n = 0;
    struct netif *nif;
    LOCK_TCPIP_CORE();
    for (int tunnels = 0; tunnels < 2; tunnels++) {
        NETIF_FOREACH(nif)
        {
            const bool tunnel = nif->name[0] == 'w' && nif->name[1] == 'g';
            if (n >= max || tunnel != (tunnels == 1) || !netif_is_up(nif) ||
                !netif_is_link_up(nif)) {
                continue;
            }
            const ip4_addr_t *ip = netif_ip4_addr(nif);
            if (ip4_addr_isany(ip) || ip4_addr_isloopback(ip)) {
                continue;
            }
            out[n].ip_be = ip4_addr_get_u32(ip);
            out[n].port = s_port;
            n++;
        }
    }
    UNLOCK_TCPIP_CORE();
    return n;
}

static void fail(int *status, int code, char *err, size_t err_len, const char *msg)
{
    *status = code;
    snprintf(err, err_len, "%s", msg);
}

/*
 * A free slot. When both are taken the oldest session is closed for the new
 * one - a console reloaded twice leaves its old sessions behind for a while -
 * and the receive task frees it within a tick.
 */
static sess_t *take_slot(void)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)TICK_MS * 10 * 1000;
    bool killed = false;
    for (;;) {
        lock();
        sess_t *oldest = NULL;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            sess_t *s = &s_sess[i];
            if (s->st == ST_FREE) {
                s->st = ST_CLAIMED;
                unlock();
                return s;
            }
            if (s->st != ST_DEAD && s->st != ST_CLAIMED &&
                (!oldest || s->created_us < oldest->created_us)) {
                oldest = s;
            }
        }
        if (!killed && oldest) {
            session_kill(oldest, "replaced by a newer one");
            killed = true;
        }
        unlock();
        const int64_t left = deadline - esp_timer_get_time();
        if (left <= 0 || (!killed && !oldest)) {
            return NULL;
        }
        (void)xSemaphoreTake(s_freed, pdMS_TO_TICKS(left / 1000 + 1));
    }
}

esp_err_t kvm_webrtc_offer(const char *offer, const uint8_t owner[32], char **answer,
                           uint32_t *id, int *status, char *err, size_t err_len)
{
    *answer = NULL;
    if (!s_ready) {
        fail(status, 503, err, err_len, "WebRTC is not ready yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (!kvm_cap_active(KVM_CAP_WEBRTC)) {
        fail(status, 409, err, err_len, "WebRTC is switched off on this device");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (video_frame_payload() == VIDEO_PAYLOAD_JPEG) {
        fail(status, 409, err, err_len, "the device is sending MJPEG; WebRTC carries H.264 only");
        return ESP_ERR_NOT_SUPPORTED;
    }
    sdp_offer_t *o = calloc(1, sizeof(*o));
    if (!o) {
        fail(status, 503, err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (sdp_parse_offer(offer, o) != 0) {
        free(o);
        fail(status, 400, err, err_len,
             "the offer has no H.264 video this device can send (packetization-mode 1, "
             "Baseline), audio other than Opus, or lacks ICE credentials or a SHA-256 "
             "fingerprint");
        return ESP_ERR_INVALID_ARG;
    }

    sess_t *s = take_slot();
    if (!s) {
        free(o);
        fail(status, 503, err, err_len, "too many WebRTC sessions");
        return ESP_ERR_NO_MEM;
    }
    /* Built outside the lock: the slot is claimed (ST_ICE) but has no ufrag
       yet, so no packet can match it. */
    sess_t n = {0};
    /* Internal RAM: the AES and SHA engines read it by DMA, and from PSRAM
       each packet first went through a bounce buffer. */
    n.pkt = heap_caps_malloc(PKT_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    n.ring = heap_caps_calloc(RING, PKT_CAP, MALLOC_CAP_SPIRAM);
    random_token(n.ufrag, sizeof(n.ufrag) - 1);
    random_token(n.pwd, sizeof(n.pwd) - 1);
    n.ice = wr_hmac_new((const uint8_t *)n.pwd, strlen(n.pwd));
    char *ans = malloc(4096);
    if (!n.pkt || !n.ring || !n.ice || !ans) {
        heap_caps_free(n.pkt);
        heap_caps_free(n.ring);
        wr_hmac_free(n.ice);
        free(ans);
        free(o);
        lock();
        s->st = ST_FREE;
        unlock();
        fail(status, 503, err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    memcpy(n.owner, owner, 32);
    memcpy(n.remote_fp, o->fingerprint, 32);
    n.rtp.ssrc = esp_random();
    n.rtp.seq = (uint16_t)esp_random();
    n.rtp.pt = (uint8_t)o->pt;
    n.rtp.max_payload = RTP_PAYLOAD;
    n.rtp.playout_ext = (uint8_t)o->playout_ext;
    if (o->has_data) {
        n.data = true;
        sctp_init(&n.sctp, 5000, esp_random(), esp_random());
    }
    if (o->has_audio) {
        n.audio = true;
        n.audio_pt = (uint8_t)o->audio_pt;
        n.audio_ssrc = esp_random();
        n.audio_seq = (uint16_t)esp_random();
        n.audio_ts0 = esp_random();
    }
    n.created_us = n.seen_us = n.owner_checked_us = esp_timer_get_time();

    sdp_candidate_t cands[6];
    const int cc = candidates(cands, 6);
    const sdp_answer_params_t p = {.ufrag = n.ufrag, .pwd = n.pwd,
                                   .fingerprint = s_cert.fingerprint, .ssrc = n.rtp.ssrc,
                                   .audio_ssrc = n.audio_ssrc,
                                   .cands = cands, .cand_count = cc};
    const int alen = sdp_write_answer(o, &p, ans, 4096);
    free(o);
    if (alen < 0 || cc == 0) {
        heap_caps_free(n.pkt);
        heap_caps_free(n.ring);
        wr_hmac_free(n.ice);
        free(ans);
        lock();
        s->st = ST_FREE;
        unlock();
        fail(status, 500, err, err_len, cc ? "the answer did not fit" : "no IPv4 address yet");
        return ESP_FAIL;
    }

    video_frame_viewer_enter(); /* the encoder warms up while ICE and DTLS run */
    lock();
    n.id = s_next_id++;
    n.st = ST_ICE;
    *s = n;
    unlock();
    ESP_LOGI(TAG, "session %u: answered, %d candidate(s) on UDP %u%s", (unsigned)n.id, cc,
             (unsigned)s_port, n.audio ? ", with audio" : "");
    *id = n.id;
    *answer = ans;
    *status = 200;
    return ESP_OK;
}

void kvm_webrtc_close(uint32_t id, const uint8_t owner[32])
{
    if (!s_mu) {
        return;
    }
    lock();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].id == id && s_sess[i].st != ST_FREE && memcmp(s_sess[i].owner, owner, 32) == 0) {
            session_kill(&s_sess[i], "the console closed it");
        }
    }
    unlock();
}

void kvm_webrtc_sound(uint32_t id, const uint8_t owner[32], bool on)
{
    if (!s_mu) {
        return;
    }
    lock();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].id == id && s_sess[i].st != ST_FREE && s_sess[i].audio &&
            memcmp(s_sess[i].owner, owner, 32) == 0) {
            s_sess[i].audio_on = on;
        }
    }
    unlock();
}

int kvm_webrtc_count(void)
{
    if (!s_mu) {
        return 0;
    }
    int n = 0;
    lock();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        n += s_sess[i].st != ST_FREE && s_sess[i].st != ST_DEAD;
    }
    unlock();
    return n;
}

void kvm_webrtc_set_input(kvm_webrtc_input_fn fn)
{
    s_input = fn;
}
