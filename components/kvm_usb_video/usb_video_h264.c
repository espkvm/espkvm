/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_video_h264.h"

typedef struct {
    const uint8_t *p, *end;
    unsigned byte, left, zeros;
    bool bad;
} rbsp_t;

static unsigned bit(rbsp_t *r)
{
    if (!r->left) {
        if (!r->bad && r->p != r->end && r->zeros == 2 && *r->p == 3) {
            r->p++;
            r->zeros = 0;
            if (r->p == r->end || *r->p > 3) {
                r->bad = true;
            }
        }
        if (r->bad || r->p == r->end) {
            r->bad = true;
            return 0;
        }
        r->byte = *r->p++;
        r->zeros = r->byte ? 0 : r->zeros + 1;
        r->left = 8;
    }
    return (r->byte >> --r->left) & 1u;
}

static uint32_t bits(rbsp_t *r, unsigned n)
{
    uint32_t value = 0;
    while (n--) {
        value = (value << 1) | bit(r);
    }
    return value;
}

static uint32_t ue(rbsp_t *r)
{
    unsigned zeros = 0;
    while (!bit(r)) {
        if (r->bad || ++zeros > 31) {
            r->bad = true;
            return 0;
        }
    }
    return ((1u << zeros) - 1u) + bits(r, zeros);
}

/* The screen senders use 8-bit progressive 4:2:0. High-profile encoders may
 * signal custom scaling lists before the dimensions. Walk them with fixed bounds. */
static void scaling_lists(rbsp_t *r)
{
    for (unsigned i = 0; i < 8 && !r->bad; i++) {
        if (!bit(r)) {
            continue;
        }
        unsigned next = 8;
        for (unsigned j = 0; j < (i < 6 ? 16u : 64u) && next; j++) {
            const uint32_t code = ue(r);
            if (r->bad || code > 256) {
                r->bad = true;
                return;
            }
            const int delta = code & 1u ? (int)((code + 1u) / 2u) : -(int)(code / 2u);
            next = (next + 256u + delta) & 255u;
        }
    }
}

static bool level_supported(unsigned level)
{
    switch (level) {
    case 9: case 10: case 11: case 12: case 13:
    case 20: case 21: case 22: case 30: case 31: case 32:
    case 40: case 41: case 42: case 50: case 51: case 52:
        return true;
    default:
        return false;
    }
}

static bool sps_valid(const uint8_t *p, const uint8_t *end, uint32_t max_width, uint32_t max_height)
{
    /* A four-byte start code can leave its leading zero on the preceding NAL. */
    while (end != p && !end[-1]) {
        end--;
    }
    rbsp_t r = {.p = p, .end = end};
    const unsigned profile = bits(&r, 8);
    const unsigned constraints = bits(&r, 8);
    const unsigned level = bits(&r, 8);
    if ((profile != 66 && profile != 77 && profile != 100) ||
        (constraints & 3u) || !level_supported(level) || ue(&r) > 31) {
        return false;
    }
    if (profile == 100) {
        const uint32_t chroma = ue(&r);
        const uint32_t luma_depth = ue(&r);
        const uint32_t chroma_depth = ue(&r);
        if (chroma != 1 || luma_depth || chroma_depth || bit(&r)) {
            return false;
        }
        if (bit(&r)) {
            scaling_lists(&r);
        }
    }
    if (ue(&r) > 12) { /* log2_max_frame_num_minus4 */
        return false;
    }
    const uint32_t poc = ue(&r);
    if (poc == 0) {
        if (ue(&r) > 12) { /* log2_max_pic_order_cnt_lsb_minus4 */
            return false;
        }
    } else if (poc == 1) {
        bit(&r);
        ue(&r); /* signed offsets have the same wire length as ue(v) */
        ue(&r);
        const uint32_t cycle = ue(&r);
        if (cycle > 255) {
            return false;
        }
        for (uint32_t i = 0; i < cycle && !r.bad; i++) {
            ue(&r);
        }
    } else if (poc != 2) {
        return false;
    }
    if (ue(&r) > 16) { /* max_num_ref_frames */
        return false;
    }
    bit(&r);
    const uint32_t width_mbs = ue(&r), height_mbs = ue(&r);
    if (width_mbs >= (max_width + 15u) / 16u || height_mbs >= (max_height + 15u) / 16u || !bit(&r)) {
        return false;
    }
    bit(&r);
    const bool cropped = bit(&r);
    const uint64_t left = cropped ? ue(&r) : 0;
    const uint64_t right = cropped ? ue(&r) : 0;
    const uint64_t top = cropped ? ue(&r) : 0;
    const uint64_t bottom = cropped ? ue(&r) : 0;
    const uint64_t crop_x = 2u * (left + right), crop_y = 2u * (top + bottom);
    const uint32_t coded_width = (width_mbs + 1u) * 16u, coded_height = (height_mbs + 1u) * 16u;
    bit(&r); /* vui_parameters_present_flag */
    return !r.bad && crop_x < coded_width && crop_y < coded_height &&
           coded_width - crop_x <= max_width && coded_height - crop_y <= max_height;
}

static const uint8_t *start_code(const uint8_t *data, const uint8_t *end)
{
    for (; end - data >= 3; data++) {
        if (!data[0] && !data[1] && data[2] == 1) {
            return data;
        }
    }
    return end;
}

bool usb_video_h264_access_unit(const uint8_t *data, size_t len, uint32_t flags,
                                uint32_t max_width, uint32_t max_height)
{
    const uint8_t *end = data + len;
    const uint8_t *nal = start_code(data, end);
    bool sps = false, pps = false, vcl = false;
    for (const uint8_t *p = data; p != nal; p++) {
        if (*p) {
            return false;
        }
    }
    while (nal != end) {
        nal += 3;
        if (nal == end || (*nal & 0x80u)) {
            return false;
        }
        const uint8_t type = *nal & 31u;
        const uint8_t *next = start_code(nal + 1, end);
        if (type == 7) {
            if (!sps_valid(nal + 1, next, max_width, max_height)) {
                return false;
            }
            sps = true;
        } else if (type == 8) {
            pps = true;
        } else if (!vcl && type >= 1 && type <= 5) {
            if (type == 5 ? flags != 3 || !sps || !pps : flags != 0 && (flags != 2 || !sps || !pps)) {
                return false;
            }
            vcl = true;
        }
        nal = next;
    }
    return vcl;
}
