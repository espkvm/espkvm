/*
 * The device's SPS fix, on a real SPS from a Function EV at 1080p: the result
 * must be byte for byte what a separate parser checked (the timing kept, the
 * signal type full or limited range with BT.601 and sRGB, no reordering), the
 * rest of the access unit must not move, and a second pass must change nothing.
 */
#include <stdio.h>
#include <string.h>

#include "h264_sps.h"

static int fails;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            fails++;                                                   \
        }                                                              \
    } while (0)

static size_t from_hex(const char *s, unsigned char *out)
{
    size_t n = 0;
    while (s[0] && s[1]) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (unsigned char)v;
        s += 2;
    }
    return n;
}

int main(void)
{
    unsigned char sps[64], want[64], pps[8], idr[8], au[256];
    const size_t sps_n = from_hex("6742c02995a01e008979610000030001000003003c84", sps);
    const size_t want_n =
        from_hex("6742c02995a01e00897966e021a0c8000003000800000301e478442350", want);
    unsigned char want_ltd[64];
    from_hex("6742c02995a01e00897966a021a0c8000003000800000301e478442350", want_ltd);
    const size_t pps_n = from_hex("68ce3c80", pps);
    const size_t idr_n = from_hex("6588840021ff", idr);
    const unsigned char sc[4] = {0, 0, 0, 1};

    size_t len = 0;
    memcpy(au + len, sc, 4), len += 4;
    memcpy(au + len, sps, sps_n), len += sps_n;
    memcpy(au + len, sc, 4), len += 4;
    memcpy(au + len, pps, pps_n), len += pps_n;
    memcpy(au + len, sc, 4), len += 4;
    memcpy(au + len, idr, idr_n), len += idr_n;

    unsigned char au2[256];
    memcpy(au2, au, len);
    const size_t out_ltd = h264_sps_patch(au2, len, sizeof(au2), false);
    CHECK(out_ltd == len - sps_n + want_n);
    CHECK(memcmp(au2 + 4, want_ltd, want_n) == 0);

    const size_t out = h264_sps_patch(au, len, sizeof(au), true);
    CHECK(out == len - sps_n + want_n);
    CHECK(memcmp(au + 4, want, want_n) == 0);
    CHECK(memcmp(au + 4 + want_n, sc, 4) == 0);
    CHECK(memcmp(au + 8 + want_n, pps, pps_n) == 0);
    CHECK(memcmp(au + out - idr_n, idr, idr_n) == 0);
    CHECK(h264_sps_patch(au, out, sizeof(au), true) == out);

    /* No room: left alone. */
    unsigned char tight[64];
    memcpy(tight, sc, 4);
    memcpy(tight + 4, sps, sps_n);
    CHECK(h264_sps_patch(tight, 4 + sps_n, 4 + sps_n, true) == 4 + sps_n);

    /* A P-frame: nothing to do. */
    unsigned char p[] = {0, 0, 0, 1, 0x41, 0x9a, 0x02};
    CHECK(h264_sps_patch(p, sizeof(p), sizeof(p) + 32, true) == sizeof(p));

    printf(fails ? "%d FAILED\n" : "h264 sps: all passed\n", fails);
    return fails ? 1 : 0;
}
