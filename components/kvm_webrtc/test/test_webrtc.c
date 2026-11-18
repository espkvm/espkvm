/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The WebRTC pieces that are plain enough for a host: SRTP and SRTCP, STUN,
 * the H.264 packetizer and the SDP reader. Vectors come from gen_vectors.py.
 * run.sh builds this with AddressSanitizer, and every parser is also fed each
 * truncation of its input.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtp_h264.h"
#include "sdp.h"
#include "sctp.h"
#include "sps_patch.h"
#include "srtp.h"
#include "stun.h"
#include "vectors.h"
#include "wr_crypto.h"

static int fails;

#define CHECK(c)                                                \
    do {                                                        \
        if (!(c)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                            \
        }                                                       \
    } while (0)

static void test_aes(void)
{
    /* FIPS-197 C.1: one block of counter mode is one block of AES. */
    uint8_t key[16], pt[16], out[16];
    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
        pt[i] = (uint8_t)(i * 0x11);
    }
    static const uint8_t want[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                     0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    wr_aes_t *a = wr_aes_new(key);
    wr_aes_ctr(a, pt, NULL, out, 16);
    wr_aes_free(a);
    CHECK(memcmp(out, want, 16) == 0);
}

static void test_srtp(void)
{
    srtp_ctx_t c;
    uint8_t buf[256];

    CHECK(srtp_init(&c, V_MK, V_MS) == 0);
    memcpy(buf, V_RTP, sizeof(V_RTP));
    CHECK(srtp_protect(&c, buf, sizeof(V_RTP), sizeof(buf)) == (int)sizeof(V_SRTP));
    CHECK(memcmp(buf, V_SRTP, sizeof(V_SRTP)) == 0);
    /* No room for the tag. */
    memcpy(buf, V_RTP, sizeof(V_RTP));
    CHECK(srtp_protect(&c, buf, sizeof(V_RTP), sizeof(V_RTP) + 9) == -1);
    srtp_free(&c);

    /* A header extension is authenticated but left clear. */
    CHECK(srtp_init(&c, V_MK, V_MS) == 0);
    memcpy(buf, V_RTP_EXT, sizeof(V_RTP_EXT));
    CHECK(srtp_protect(&c, buf, sizeof(V_RTP_EXT), sizeof(buf)) == (int)sizeof(V_SRTP_EXT));
    CHECK(memcmp(buf, V_SRTP_EXT, sizeof(V_SRTP_EXT)) == 0);
    /* An extension longer than the packet is refused. */
    memcpy(buf, V_RTP_EXT, 16);
    buf[15] = 0x40;
    CHECK(srtp_protect(&c, buf, 16, sizeof(buf)) == -1);
    srtp_free(&c);

    /* Across a sequence wrap the rollover counter goes to 1. */
    CHECK(srtp_init(&c, V_MK, V_MS) == 0);
    memcpy(buf, V_RTP, sizeof(V_RTP));
    buf[2] = 0xff;
    buf[3] = 0xf0;
    srtp_protect(&c, buf, sizeof(V_RTP), sizeof(buf));
    memcpy(buf, V_RTP_WRAP, sizeof(V_RTP_WRAP));
    CHECK(srtp_protect(&c, buf, sizeof(V_RTP_WRAP), sizeof(buf)) == (int)sizeof(V_SRTP_WRAP));
    CHECK(memcmp(buf, V_SRTP_WRAP, sizeof(V_SRTP_WRAP)) == 0);
    srtp_free(&c);

    /* SRTCP out, index 0. */
    CHECK(srtp_init(&c, V_MK, V_MS) == 0);
    memcpy(buf, V_RTCP, sizeof(V_RTCP));
    CHECK(srtcp_protect(&c, buf, sizeof(V_RTCP), sizeof(buf)) == (int)sizeof(V_SRTCP));
    CHECK(memcmp(buf, V_SRTCP, sizeof(V_SRTCP)) == 0);
    srtp_free(&c);

    /* SRTCP in: accepted once, then a replay. */
    CHECK(srtp_init(&c, V_MK, V_MS) == 0);
    memcpy(buf, V_SRTCP7, sizeof(V_SRTCP7));
    CHECK(srtcp_unprotect(&c, buf, sizeof(V_SRTCP7)) == (int)sizeof(V_RTCP));
    CHECK(memcmp(buf, V_RTCP, sizeof(V_RTCP)) == 0);
    memcpy(buf, V_SRTCP7, sizeof(V_SRTCP7));
    CHECK(srtcp_unprotect(&c, buf, sizeof(V_SRTCP7)) == -1);
    /* An older index inside the window is still fine. */
    memcpy(buf, V_SRTCP, sizeof(V_SRTCP));
    CHECK(srtcp_unprotect(&c, buf, sizeof(V_SRTCP)) == (int)sizeof(V_RTCP));
    /* Any flipped bit and any truncation is refused. */
    for (size_t i = 0; i < sizeof(V_SRTCP7); i++) {
        memcpy(buf, V_SRTCP7, sizeof(V_SRTCP7));
        buf[i] ^= 0x01;
        srtp_ctx_t f;
        srtp_init(&f, V_MK, V_MS);
        CHECK(srtcp_unprotect(&f, buf, sizeof(V_SRTCP7)) == -1);
        srtp_free(&f);
    }
    for (size_t n = 0; n < sizeof(V_SRTCP7); n++) {
        uint8_t *t = malloc(n ? n : 1);
        memcpy(t, V_SRTCP7, n);
        srtp_ctx_t f;
        srtp_init(&f, V_MK, V_MS);
        CHECK(srtcp_unprotect(&f, t, n) == -1);
        srtp_free(&f);
        free(t);
    }
    srtp_free(&c);

    /* Two senders, each counting its own index from 0: both are taken, and a
       replay is still a replay for each of them. */
    {
        srtp_ctx_t a, b, in;
        uint8_t pa[3][64], pb[3][64];
        int la[3], lb[3];
        srtp_init(&a, V_MK, V_MS);
        srtp_init(&b, V_MK, V_MS);
        srtp_init(&in, V_MK, V_MS);
        for (int i = 0; i < 3; i++) {
            memcpy(pa[i], V_RTCP, sizeof(V_RTCP));
            la[i] = srtcp_protect(&a, pa[i], sizeof(V_RTCP), sizeof(pa[i]));
            memcpy(pb[i], V_RTCP, sizeof(V_RTCP));
            pb[i][7] ^= 0x55; /* another SSRC */
            lb[i] = srtcp_protect(&b, pb[i], sizeof(V_RTCP), sizeof(pb[i]));
        }
        uint8_t t[64];
        for (int i = 0; i < 3; i++) {
            memcpy(t, pa[i], (size_t)la[i]);
            CHECK(srtcp_unprotect(&in, t, (size_t)la[i]) == (int)sizeof(V_RTCP));
        }
        for (int i = 0; i < 3; i++) {
            memcpy(t, pb[i], (size_t)lb[i]);
            CHECK(srtcp_unprotect(&in, t, (size_t)lb[i]) == (int)sizeof(V_RTCP));
        }
        memcpy(t, pb[1], (size_t)lb[1]);
        CHECK(srtcp_unprotect(&in, t, (size_t)lb[1]) == -1);
        memcpy(t, pa[2], (size_t)la[2]);
        CHECK(srtcp_unprotect(&in, t, (size_t)la[2]) == -1);
        srtp_free(&a);
        srtp_free(&b);
        srtp_free(&in);
    }
}

static void test_stun(void)
{
    wr_hmac_t *pwd = wr_hmac_new(V_STUN_PWD, sizeof(V_STUN_PWD));
    stun_req_t req;
    CHECK(stun_is(V_STUN_REQ, sizeof(V_STUN_REQ)));
    CHECK(stun_check_binding(V_STUN_REQ, sizeof(V_STUN_REQ), "dev1", pwd, &req) == 0);
    CHECK(req.use_candidate);
    CHECK(req.txid[0] == 1 && req.txid[11] == 12);
    CHECK(stun_check_binding(V_STUN_REQ, sizeof(V_STUN_REQ), "dev2", pwd, &req) == -1);
    char uf[16];
    CHECK(stun_local_ufrag(V_STUN_REQ, sizeof(V_STUN_REQ), uf, sizeof(uf)) == 0);
    CHECK(strcmp(uf, "dev1") == 0);
    CHECK(stun_local_ufrag(V_STUN_REQ, sizeof(V_STUN_REQ), uf, 4) == -1);
    CHECK(stun_check_binding(V_STUN_REQ, sizeof(V_STUN_REQ), "dev", pwd, &req) == -1);

    wr_hmac_t *other = wr_hmac_new((const uint8_t *)"wrong", 5);
    CHECK(stun_check_binding(V_STUN_REQ, sizeof(V_STUN_REQ), "dev1", other, &req) == -1);
    wr_hmac_free(other);

    uint8_t buf[sizeof(V_STUN_REQ)];
    for (size_t i = 0; i < sizeof(V_STUN_REQ); i++) {
        memcpy(buf, V_STUN_REQ, sizeof(buf));
        buf[i] ^= 0x40;
        CHECK(stun_check_binding(buf, sizeof(buf), "dev1", pwd, &req) == -1);
    }
    for (size_t n = 0; n < sizeof(V_STUN_REQ); n++) {
        uint8_t *t = malloc(n ? n : 1);
        memcpy(t, V_STUN_REQ, n);
        CHECK(stun_check_binding(t, n, "dev1", pwd, &req) == -1);
        stun_local_ufrag(t, n, uf, sizeof(uf));
        free(t);
    }

    uint8_t resp[STUN_RESPONSE_LEN];
    const uint8_t ip[4] = {192, 168, 1, 20};
    uint32_t ip_be;
    memcpy(&ip_be, ip, 4);
    const uint8_t port[2] = {0xc3, 0x50}; /* 50000 */
    uint16_t port_be;
    memcpy(&port_be, port, 2);
    CHECK(stun_build_success(req.txid, ip_be, port_be, pwd, resp, sizeof(resp)) ==
          sizeof(V_STUN_RESP));
    CHECK(memcmp(resp, V_STUN_RESP, sizeof(V_STUN_RESP)) == 0);
    CHECK(stun_build_success(req.txid, ip_be, port_be, pwd, resp, sizeof(resp) - 1) == 0);
    wr_hmac_free(pwd);
}

typedef struct {
    int n;
    uint8_t marker[16];
    uint8_t payload[16][1300];
    size_t len[16];
} cap_t;

static int capture(void *ctx, uint8_t *pkt, size_t len)
{
    cap_t *c = ctx;
    if (c->n >= 16) {
        return -1;
    }
    c->marker[c->n] = pkt[1] >> 7;
    c->len[c->n] = len - RTP_HDR_LEN;
    memcpy(c->payload[c->n], pkt + RTP_HDR_LEN, len - RTP_HDR_LEN);
    c->n++;
    return 0;
}

static uint8_t s_kept[64];
static size_t s_kept_len;

static int keep(void *ctx, uint8_t *pkt, size_t len)
{
    (void)ctx;
    if (!s_kept_len && len <= sizeof(s_kept)) {
        memcpy(s_kept, pkt, len);
        s_kept_len = len;
    }
    return 0;
}

static void test_rtp(void)
{
    uint8_t au[4000];
    size_t n = 0;
    const uint8_t aud[] = {0, 0, 0, 1, 0x09, 0xf0};
    memcpy(au + n, aud, sizeof(aud));
    n += sizeof(aud);
    const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x1f, 1, 2, 3, 4};
    memcpy(au + n, sps, sizeof(sps));
    n += sizeof(sps);
    const uint8_t pps[] = {0, 0, 1, 0x68, 0xce, 0x38, 0x80};
    memcpy(au + n, pps, sizeof(pps));
    n += sizeof(pps);
    const uint8_t idr[] = {0, 0, 0, 1, 0x65};
    memcpy(au + n, idr, sizeof(idr));
    n += sizeof(idr);
    const size_t idr_at = n - 1;
    for (int i = 0; i < 3000; i++) {
        au[n++] = (uint8_t)(i * 7 + 1) | 1; /* no accidental start codes */
    }

    rtp_h264_t st = {.ssrc = 0x11223344, .seq = 0xfffe, .pt = 102, .max_payload = 1200};
    uint8_t buf[RTP_HDR_LEN + 1200];
    static cap_t c;
    memset(&c, 0, sizeof(c));
    const int got = rtp_h264_send(&st, au, n, 90000, buf, capture, &c);
    /* SPS, PPS, then the IDR in three FU-A fragments; the AUD is dropped. */
    CHECK(got == 5);
    CHECK(c.n == 5);
    CHECK(st.seq == 3); /* wrapped */
    CHECK(c.len[0] == 8 && c.payload[0][0] == 0x67);
    CHECK(c.len[1] == 4 && c.payload[1][0] == 0x68);
    CHECK(c.payload[2][0] == (0x60 | 28) && c.payload[2][1] == (0x80 | 5));
    CHECK(c.payload[3][1] == 5);
    CHECK(c.payload[4][1] == (0x40 | 5));
    CHECK(!c.marker[0] && !c.marker[3] && c.marker[4]);
    /* The fragments put back together are the NAL unit minus its header. */
    uint8_t joined[4000];
    size_t jn = 0;
    for (int i = 2; i < 5; i++) {
        memcpy(joined + jn, c.payload[i] + 2, c.len[i] - 2);
        jn += c.len[i] - 2;
    }
    CHECK(jn == 3000 && memcmp(joined, au + idr_at + 1, 3000) == 0);

    /* With playout-delay on, every packet carries the extension: X set, the
       one-byte-header block, id 7, min and max 0; the payload moves past it. */
    rtp_h264_t ex = {.ssrc = 1, .seq = 0, .pt = 102, .max_payload = 1200, .playout_ext = 7};
    uint8_t ebuf[RTP_HDR_LEN + RTP_EXT_LEN + 1200];
    s_kept_len = 0;
    CHECK(rtp_h264_send(&ex, sps, sizeof(sps), 0, ebuf, keep, NULL) == 1);
    static const uint8_t want_ext[] = {0xbe, 0xde, 0x00, 0x01, 0x72, 0x00, 0x00, 0x00};
    CHECK(s_kept_len == RTP_HDR_LEN + RTP_EXT_LEN + 8);
    CHECK(s_kept[0] == 0x90 && memcmp(s_kept + RTP_HDR_LEN, want_ext, sizeof(want_ext)) == 0);
    CHECK(s_kept[RTP_HDR_LEN + RTP_EXT_LEN] == 0x67);

    /* Stopping the emitter stops the frame. */
    static cap_t full;
    memset(&full, 0, sizeof(full));
    full.n = 16;
    CHECK(rtp_h264_send(&st, au, n, 0, buf, capture, &full) == -1);

    /* Junk with no start code sends nothing; truncations do not overrun. */
    memset(&c, 0, sizeof(c));
    const uint8_t junk[] = {1, 2, 3, 4, 5};
    CHECK(rtp_h264_send(&st, junk, sizeof(junk), 0, buf, capture, &c) == 0);
    for (size_t k = 0; k < 40; k++) {
        memset(&c, 0, sizeof(c));
        uint8_t *t = malloc(k ? k : 1);
        memcpy(t, au, k);
        rtp_h264_send(&st, t, k, 0, buf, capture, &c);
        free(t);
    }
}

static const char *OFFER =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=extmap-allow-mixed\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 127\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:brws\r\n"
    "a=ice-pwd:browserpasswordbrowserpw\r\n"
    "a=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 "
    "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=rtcp-fb:102 goog-remb\r\n"
    "a=rtcp-fb:102 transport-cc\r\n"
    "a=rtcp-fb:102 ccm fir\r\n"
    "a=rtcp-fb:102 nack\r\n"
    "a=rtcp-fb:102 nack pli\r\n"
    "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f\r\n"
    "a=rtpmap:103 rtx/90000\r\n"
    "a=fmtp:103 apt=102\r\n"
    "a=rtpmap:127 H264/90000\r\n"
    "a=rtcp-fb:127 nack\r\n"
    "a=fmtp:127 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n";

static void test_sdp(void)
{
    sdp_offer_t o;
    CHECK(sdp_parse_offer(OFFER, &o) == 0);
    /* Constrained Baseline wins over plain Baseline. */
    CHECK(o.pt == 127);
    CHECK(strcmp(o.plid, "42e01f") == 0);
    CHECK(o.fb_nack && !o.fb_pli && !o.fb_remb);
    CHECK(strcmp(o.ufrag, "brws") == 0);
    CHECK(strcmp(o.pwd, "browserpasswordbrowserpw") == 0);
    CHECK(strcmp(o.mid, "0") == 0);
    CHECK(o.has_fingerprint && o.fingerprint[0] == 0x01 && o.fingerprint[31] == 0xef);
    CHECK(o.rtcp_rsize);

    uint8_t fp[32] = {0xaa};
    const uint8_t ip[4] = {192, 168, 1, 146};
    sdp_candidate_t cand = {.port = 50000};
    memcpy(&cand.ip_be, ip, 4);
    sdp_answer_params_t p = {.ufrag = "dev1", .pwd = "devicepassworddevicepw", .fingerprint = fp,
                             .ssrc = 1234, .cands = &cand, .cand_count = 1};
    char ans[2048];
    const int n = sdp_write_answer(&o, &p, ans, sizeof(ans));
    CHECK(n > 0);
    CHECK(strstr(ans, "a=ice-lite\r\n") != NULL);
    CHECK(strstr(ans, "m=video 9 UDP/TLS/RTP/SAVPF 127\r\n") != NULL);
    CHECK(strstr(ans, "a=setup:passive\r\n") != NULL);
    CHECK(strstr(ans, "a=fingerprint:sha-256 AA:00:") != NULL);
    CHECK(strstr(ans, "192.168.1.146 50000 typ host") != NULL);
    CHECK(strstr(ans, "profile-level-id=42e01f") != NULL);
    CHECK(sdp_write_answer(&o, &p, ans, 100) == -1);

    /* Without Constrained Baseline the plain Baseline type is taken. */
    char *alt = strdup(OFFER);
    char *at = strstr(alt, "42e01f");
    memcpy(at, "640c1f", 6);
    CHECK(sdp_parse_offer(alt, &o) == 0 && o.pt == 102 && o.fb_pli && o.fb_remb && o.fb_fir);
    free(alt);

    /* Audio that is not Opus, no fingerprint, a bad mid: refused. */
    char two[4096];
    snprintf(two, sizeof(two), "%sm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:1\r\n", OFFER);
    CHECK(sdp_parse_offer(two, &o) == -1);

    /* Opus after the video: taken, answered in the same order and bundled. */
    snprintf(two, sizeof(two),
             "%sm=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\na=mid:1\r\na=rtpmap:0 PCMU/8000\r\n"
             "a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10;useinbandfec=1\r\n",
             OFFER);
    CHECK(sdp_parse_offer(two, &o) == 0 && o.has_audio && !o.audio_first && o.audio_pt == 111 &&
          strcmp(o.audio_mid, "1") == 0);
    {
        sdp_answer_params_t pa = p;
        pa.audio_ssrc = 0x01020304;
        char big[4096];
        CHECK(sdp_write_answer(&o, &pa, big, sizeof(big)) > 0);
        const char *mv = strstr(big, "m=video "), *ma = strstr(big, "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n");
        CHECK(mv && ma && mv < ma);
        CHECK(strstr(big, "a=rtpmap:111 opus/48000/2\r\n") != NULL);
        CHECK(strstr(big, "a=ssrc:16909060 cname:espkvm\r\n") != NULL);
        CHECK(strstr(big, "a=group:BUNDLE 0 1\r\n") != NULL);
        CHECK(sdp_write_answer(&o, &pa, big, 900) == -1);
    }
    /* A data channel after the two: answered last, on the browser's mids. */
    char three[4400];
    snprintf(three, sizeof(three),
             "%sm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:2\r\na=sctp-port:5001\r\n",
             two);
    CHECK(sdp_parse_offer(three, &o) == 0 && o.has_data && o.sctp_port == 5001 &&
          strcmp(o.data_mid, "2") == 0);
    {
        sdp_answer_params_t pa = p;
        char big[4096];
        CHECK(sdp_write_answer(&o, &pa, big, sizeof(big)) > 0);
        const char *ma = strstr(big, "m=audio "), *md = strstr(big, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n");
        CHECK(ma && md && ma < md);
        CHECK(strstr(big, "a=group:BUNDLE 0 1 2\r\n") != NULL);
        CHECK(strstr(big, "a=sctp-port:5000\r\n") != NULL);
    }
    /* A data channel without a mid, a fourth m-line, or audio sharing the
       video's mid: refused. */
    snprintf(three, sizeof(three), "%sm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n", two);
    CHECK(sdp_parse_offer(three, &o) == -1);
    char four[4600];
    snprintf(four, sizeof(four),
             "%sm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:2\r\nm=video 9 UDP/TLS/RTP/SAVPF 127\r\n",
             two);
    CHECK(sdp_parse_offer(four, &o) == -1);
    char *same = strdup(two);
    memcpy(strstr(same, "a=mid:1"), "a=mid:0", 7);
    CHECK(sdp_parse_offer(same, &o) == -1);
    free(same);
    for (size_t n = 0; n < strlen(two); n++) {
        char *t = strndup(two, n);
        (void)sdp_parse_offer(t, &o);
        free(t);
    }
    char *nofp = strdup(OFFER);
    memcpy(strstr(nofp, "a=fingerprint"), "a=fingerprXnt", 13);
    CHECK(sdp_parse_offer(nofp, &o) == -1);
    free(nofp);
    /* The playout-delay extension is taken, with or without a direction. */
    CHECK(sdp_parse_offer(OFFER, &o) == 0 && o.playout_ext == 0);
    char ext[4096];
    snprintf(ext, sizeof(ext),
             "%sa=extmap:12/recvonly http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n",
             OFFER);
    CHECK(sdp_parse_offer(ext, &o) == 0 && o.playout_ext == 12);
    CHECK(sdp_write_answer(&o, &p, ans, sizeof(ans)) > 0);
    CHECK(strstr(ans, "a=extmap:12 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"));
    snprintf(ext, sizeof(ext),
             "%sa=extmap:15 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n", OFFER);
    CHECK(sdp_parse_offer(ext, &o) == 0 && o.playout_ext == 0); /* two-byte ids: not used */

    /* Other hashes next to the SHA-256 one do not cancel it. */
    char more[4096];
    snprintf(more, sizeof(more), "%sa=fingerprint:sha-384 A4:79:19\r\n", OFFER);
    CHECK(sdp_parse_offer(more, &o) == 0 && o.has_fingerprint && o.fingerprint[0] == 0x01);
    char *badmid = strdup(OFFER);
    memcpy(strstr(badmid, "a=mid:0"), "a=mid:<", 7);
    CHECK(sdp_parse_offer(badmid, &o) == -1);
    free(badmid);
    CHECK(sdp_parse_offer("", &o) == -1);
    CHECK(sdp_parse_offer("garbage\nm=\na=rtpmap:999999999 H264/90000\n", &o) == -1);

    /* Every truncation parses without reading past the end. */
    const size_t len = strlen(OFFER);
    for (size_t k = 0; k < len; k++) {
        char *t = malloc(k + 1);
        memcpy(t, OFFER, k);
        t[k] = 0;
        sdp_parse_offer(t, &o);
        free(t);
    }
}

/* The same SPS and the same answer as web/test/spsPatch.test.ts gives. */

/* ---- SCTP ---- */

static int s_msgs;
static uint8_t s_msg[16];
static size_t s_msg_len;

static void on_msg(void *ctx, uint16_t stream, uint32_t ppid, const uint8_t *d, size_t n)
{
    (void)ctx;
    CHECK(stream == 0 && ppid == SCTP_PPID_BINARY);
    s_msgs++;
    s_msg_len = n < sizeof(s_msg) ? n : sizeof(s_msg);
    memcpy(s_msg, d, s_msg_len);
}

static size_t pkt_hdr(uint8_t *p, uint32_t vtag)
{
    memset(p, 0, 12);
    p[0] = 0x13; p[1] = 0x88; /* 5000 */
    p[2] = 0x13; p[3] = 0x88;
    p[4] = (uint8_t)(vtag >> 24); p[5] = (uint8_t)(vtag >> 16); p[6] = (uint8_t)(vtag >> 8); p[7] = (uint8_t)vtag;
    return 12;
}

static size_t put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    return 4;
}

static size_t data_pkt(uint8_t *p, uint32_t vtag, uint32_t tsn, uint8_t flags, const char *msg)
{
    size_t n = pkt_hdr(p, vtag);
    const size_t ml = strlen(msg);
    p[n] = 0; p[n + 1] = flags; p[n + 2] = 0; p[n + 3] = (uint8_t)(16 + ml);
    n += 4;
    n += put_be32(p + n, tsn);
    p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 0; /* stream 0, ssn 0 */
    n += put_be32(p + n, SCTP_PPID_BINARY);
    memcpy(p + n, msg, ml);
    n += (ml + 3) & ~3u;
    return n;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void test_sctp(void)
{
    CHECK(sctp_crc32c((const uint8_t *)"123456789", 9) == 0xe3069283u);

    sctp_assoc_t a;
    sctp_init(&a, 5000, 0xcafef00d, 1000);
    uint8_t in[256], out[256];

    /* INIT: tag 0, initiate tag 0x11223344, first TSN 500. */
    size_t n = pkt_hdr(in, 0);
    const uint8_t init[] = {1, 0, 0, 20, 0x11, 0x22, 0x33, 0x44, 0, 1, 0, 0, 0, 16, 0, 16, 0, 0, 1, 0xf4};
    memcpy(in + n, init, sizeof(init));
    n += sizeof(init);
    size_t r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(r > 12 && out[12] == 2);              /* INIT ACK */
    CHECK(be32(out + 4) == 0x11223344u);        /* to the browser's tag */
    CHECK(be32(out + 16) == 0xcafef00du);       /* our tag */
    {
        uint8_t c[256];
        memcpy(c, out, r);
        const uint32_t got = (uint32_t)c[8] | (uint32_t)c[9] << 8 | (uint32_t)c[10] << 16 | (uint32_t)c[11] << 24;
        memset(c + 8, 0, 4);
        CHECK(sctp_crc32c(c, r) == got);
    }
    CHECK(a.cum_tsn == 499 && !a.up);

    /* COOKIE ECHO -> COOKIE ACK. A wrong tag is ignored. */
    n = pkt_hdr(in, 0xcafef00d);
    const uint8_t echo[] = {10, 0, 0, 12, 0xca, 0xfe, 0xf0, 0x0d, 0x11, 0x22, 0x33, 0x44};
    memcpy(in + n, echo, sizeof(echo));
    n += sizeof(echo);
    r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(r == 16 && out[12] == 11 && a.up);
    pkt_hdr(in, 0xdeadbeef);
    CHECK(sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL) == 0);

    /* In order: delivered; acknowledged later (every few packets, or when
       the caller asks with sctp_sack). */
    n = data_pkt(in, 0xcafef00d, 500, 0x07, "abcdefgh");
    r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(s_msgs == 1 && s_msg_len == 8 && memcmp(s_msg, "abcdefgh", 8) == 0);
    CHECK(r == 0);
    r = sctp_sack(&a, out, sizeof(out));
    CHECK(r == 12 + 16 && out[12] == 3 && be32(out + 16) == 500);
    CHECK(sctp_sack(&a, out, sizeof(out)) == 0); /* nothing new since */
    /* The same TSN again: not delivered twice. */
    r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(s_msgs == 1 && r == 0);

    /* 502 before 501: delivered at once, a gap block reported. */
    n = data_pkt(in, 0xcafef00d, 502, 0x07, "x");
    r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(s_msgs == 2 && be32(out + 16) == 500 && out[24] == 0 && out[25] == 1);
    CHECK(out[28] == 0 && out[29] == 2 && out[30] == 0 && out[31] == 2);
    n = data_pkt(in, 0xcafef00d, 501, 0x07, "y");
    (void)sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    r = sctp_sack(&a, out, sizeof(out));
    CHECK(s_msgs == 3 && r > 12 && be32(out + 16) == 502 && out[25] == 0);

    /* A fragment is skipped, but its TSN counts. */
    n = data_pkt(in, 0xcafef00d, 503, 0x06, "frag");
    (void)sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(s_msgs == 3 && a.cum_tsn == 503);

    /* FORWARD TSN past a lost one; then a far jump. */
    n = pkt_hdr(in, 0xcafef00d);
    const uint8_t fwd[] = {192, 0, 0, 8, 0, 0, 0x01, 0xfa};
    memcpy(in + n, fwd, sizeof(fwd));
    n += sizeof(fwd);
    r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(a.cum_tsn == 506 && r > 12 && be32(out + 16) == 506);
    in[n - 4] = 0x7f;
    (void)sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(a.cum_tsn == 0x7f0001fau && a.ahead == 0);

    /* Every SCTP_SACK_EVERY packets a SACK comes on its own. */
    {
        size_t got = 0;
        for (uint32_t k = 1; k <= SCTP_SACK_EVERY; k++) {
            n = data_pkt(in, 0xcafef00d, 0x7f0001fau + k, 0x07, "m");
            if (sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL) > 0) got = k;
        }
        CHECK(got == SCTP_SACK_EVERY);
    }

    /* HEARTBEAT is echoed. */
    n = pkt_hdr(in, 0xcafef00d);
    const uint8_t hb[] = {4, 0, 0, 12, 0, 1, 0, 8, 1, 2, 3, 4};
    memcpy(in + n, hb, sizeof(hb));
    n += sizeof(hb);
    r = sctp_input(&a, in, n, out, sizeof(out), on_msg, NULL);
    CHECK(r == 24 && out[12] == 5 && memcmp(out + 16, hb + 4, 8) == 0);

    /* Every truncation and a tiny answer buffer: no reads or writes past the end. */
    n = data_pkt(in, 0xcafef00d, 0x7f000200u, 0x07, "abcdefgh");
    for (size_t k = 0; k <= n; k++) {
        uint8_t *t = malloc(k ? k : 1);
        memcpy(t, in, k);
        (void)sctp_input(&a, t, k, out, sizeof(out), on_msg, NULL);
        free(t);
    }
    CHECK(sctp_input(&a, in, n, out, 20, on_msg, NULL) == 0);
}

static void test_sps(void)
{
    static const uint8_t sps[] = {0x67, 0x42, 0xc0, 0x29, 0x95, 0xa0, 0x1e, 0x00, 0x89, 0x79, 0x61,
                                  0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x3c, 0x84};
    static const uint8_t want[] = {0x67, 0x42, 0xc0, 0x29, 0x95, 0xa0, 0x1e, 0x00, 0x89,
                                   0x79, 0x61, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00,
                                   0x03, 0x00, 0x3c, 0x8f, 0x08, 0x84, 0x6a};
    uint8_t out[64], again[64];
    const size_t n = sps_patch(sps, sizeof(sps), out, sizeof(out));
    CHECK(n == sizeof(want) && memcmp(out, want, n) == 0);
    CHECK(sps_patch(out, n, again, sizeof(again)) == 0); /* already restricted */
    CHECK(sps_patch(sps, 5, out, sizeof(out)) == 0);     /* cut short */
    CHECK(sps_patch(sps, sizeof(sps), out, 8) == 0);     /* no room */

    /* In an access unit: the PPS and the slice after it are kept. */
    static const uint8_t tail[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
                                   0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x21, 0xff};
    uint8_t au[64], au2[80];
    size_t al = 0;
    memcpy(au, "\0\0\0\1", 4);
    al = 4;
    memcpy(au + al, sps, sizeof(sps));
    al += sizeof(sps);
    memcpy(au + al, tail, sizeof(tail));
    al += sizeof(tail);
    const size_t m = sps_patch_au(au, al, au2, sizeof(au2));
    CHECK(m == al + 3);
    CHECK(memcmp(au2 + 4, want, sizeof(want)) == 0);
    CHECK(memcmp(au2 + m - sizeof(tail), tail, sizeof(tail)) == 0);
    CHECK(sps_patch_au(au2, m, au, sizeof(au)) == 0); /* nothing left to patch */
    static const uint8_t delta[] = {0, 0, 0, 1, 0x41, 0x88, 0x84, 0x00, 0x21, 0xff};
    CHECK(sps_patch_au(delta, sizeof(delta), au, sizeof(au)) == 0);
}

int main(void)
{
    test_aes();
    test_srtp();
    test_stun();
    test_rtp();
    test_sdp();
    test_sps();
    test_sctp();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("webrtc: all checks passed\n");
    return 0;
}
