#!/usr/bin/env python3
"""Generate the EDID profiles the TC358743 presents to the target machine.

Run from the repo root:

    python3 tools/gen_edid.py > components/tc358743/tc358743_edid.h

Why this is generated rather than hand-written: the byte layout is unforgiving
(two checksums, packed timing descriptors, a CEA extension), and every profile
has to respect one hard limit that is easy to forget.

    THE PIXEL CLOCK CEILING

The link is 2 MIPI lanes at 972 Mbit/s = 1.944 Gbit/s, and the capture path is
RGB888, so 24 bits leave the bridge per pixel:

    1.944e9 / 24 = 81 Mpixel/s

Every mode advertised here must stay under that, counting blanking:

    640x480@60    25.2 MHz   ok
    800x600@60    40.0 MHz   ok
    1024x768@60   65.0 MHz   ok
    1280x720@60   74.25 MHz  ok
    1920x1080@30  74.25 MHz  ok      <- the ceiling in practice
    1280x1024@60 108.0 MHz   TOO FAST
    1920x1080@60 148.5 MHz   TOO FAST

Advertising a mode above the ceiling does not degrade gracefully: the source
sends it, the CSI receiver never completes a frame, and the screen goes black.
Raising the ceiling means raising the lane rate or dropping to 16 bits per
pixel, not editing this file.
"""

import sys

# Known-good base EDID from the Waveshare/Geekworm HDMI-to-CSI adapter, as
# shipped in p4kvm. It advertises 1920x1080@30 only. Everything below is
# surgery on this block rather than a from-scratch build, so the parts that are
# known to work with real sources stay byte-identical.
BASE = bytes([
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x52, 0x62, 0x88, 0x88,
    0x00, 0x88, 0x88, 0x88, 0x1c, 0x15, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78,
    0x0a, 0x0d, 0xc9, 0xa0, 0x57, 0x47, 0x98, 0x27, 0x12, 0x48, 0x4c, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1d, 0x80, 0x18, 0x71, 0x38,
    0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x80, 0x38, 0x74, 0x00, 0x00, 0x1e,
    0x01, 0x1d, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00,
    0x80, 0x38, 0x74, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44,
    0x43, 0x44, 0x5a, 0x2d, 0x48, 0x32, 0x43, 0x20, 0x4d, 0x4f, 0x44, 0x0a,
    0x00, 0x00, 0x00, 0xfd, 0x00, 0x14, 0x78, 0x01, 0xff, 0x10, 0x00, 0x0a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0xb9, 0x02, 0x03, 0x1a, 0x71,
    0x47, 0xa2, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x23, 0x09, 0x07, 0x01,
    0x83, 0x01, 0x00, 0x00, 0x65, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x01, 0x1d,
    0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x80, 0x38,
    0x74, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40,
    0x58, 0x2c, 0x45, 0x00, 0x80, 0x38, 0x74, 0x00, 0x00, 0x1e, 0x01, 0x1d,
    0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x80, 0x38,
    0x74, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40,
    0x58, 0x2c, 0x45, 0x00, 0x80, 0x38, 0x74, 0x00, 0x00, 0x1e, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03,
])

# Established timings, byte 35 (bit -> mode)
EST1_640x480_60 = 0x20
EST1_800x600_60 = 0x01
EST1_720x400_70 = 0x80  # the text mode many BIOSes still use
# Established timings, byte 36
EST2_800x600_72 = 0x80
EST2_1024x768_60 = 0x08

# CEA-861 video identification codes, all within the pixel clock ceiling.
VIC_640x480p60 = 1
VIC_720x480p60_4_3 = 2
VIC_720x480p60_16_9 = 3
VIC_1280x720p60 = 4
VIC_1920x1080p30 = 34
VIC_NATIVE = 0x80


def std_timing(width: int, refresh: int, aspect: str) -> bytes:
    """Encode one standard timing pair (EDID 1.3 section 3.9)."""
    aspect_bits = {"16:10": 0, "4:3": 1, "5:4": 2, "16:9": 3}[aspect]
    return bytes([(width // 8) - 31, (aspect_bits << 6) | (refresh - 60)])


def checksum(block: bytes) -> int:
    return (256 - (sum(block[:127]) & 0xFF)) & 0xFF


def build_full() -> bytes:
    e = bytearray(BASE)

    # Established timings: what a firmware screen or a bare framebuffer picks.
    e[35] = EST1_640x480_60 | EST1_800x600_60 | EST1_720x400_70
    e[36] = EST2_1024x768_60 | EST2_800x600_72
    e[37] = 0x00

    # Standard timings: one useful widescreen mode, the rest stay unused (0x0101).
    e[38:40] = std_timing(1280, 60, "16:9")

    # Monitor range descriptor at offset 108: cap the maximum pixel clock at
    # 80 MHz (units of 10 MHz) so a source that reads the range instead of the
    # mode list cannot pick something the MIPI link cannot carry.
    assert e[108:113] == bytes([0x00, 0x00, 0x00, 0xFD, 0x00]), "range descriptor moved"
    e[117] = 8

    e[127] = checksum(bytes(e[:128]))

    # CEA extension: replace the padded VIC list with the modes we can accept.
    # The video data block starts at 132 (tag 0x40 | length) and is 7 bytes of
    # payload in the base EDID, which is exactly what we need.
    assert e[132] == 0x47, "video data block moved or resized"
    vics = [
        VIC_1920x1080p30 | VIC_NATIVE,
        VIC_1280x720p60,
        VIC_720x480p60_16_9,
        VIC_720x480p60_4_3,
        VIC_640x480p60,
    ]
    # Keep the block length byte honest: tag 2 (video) in the top 3 bits.
    e[132] = 0x40 | len(vics)
    e[133:133 + len(vics)] = bytes(vics)
    # The two freed bytes become a zero-length reserved block, which parsers skip.
    for i in range(133 + len(vics), 140):
        e[i] = 0x00

    e[255] = checksum(bytes(e[128:256]))
    return bytes(e)


def c_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[TC358743_EDID_TOTAL_LEN] = {{"]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    full = build_full()
    if len(full) != 256:
        print("internal error: EDID must be 256 bytes", file=sys.stderr)
        return 1

    print("/*")
    print(" * Generated by tools/gen_edid.py - do not edit by hand.")
    print(" *")
    print(" * Two profiles are offered to the target machine:")
    print(" *   full     640x480, 720x400, 800x600, 1024x768, 1280x720, 1920x1080@30")
    print(" *   1080p30  the original single-mode EDID, for sources that reject the above")
    print(" *")
    print(" * Nothing above 81 Mpixel/s may be advertised: 2 MIPI lanes at 972 Mbit/s")
    print(" * carrying RGB888 cannot deliver more. See the script for the arithmetic.")
    print(" */")
    print("#pragma once")
    print("#include <stdint.h>")
    print("#include <stddef.h>")
    print()
    print("#define TC358743_EDID_NUM_BLOCKS 2")
    print("#define TC358743_EDID_TOTAL_LEN (128 * TC358743_EDID_NUM_BLOCKS)")
    print()
    print(c_array("tc358743_edid_full", full))
    print()
    print(c_array("tc358743_edid_1080p30", BASE))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
