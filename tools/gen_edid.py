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

# 2 MIPI lanes at 972 Mbit/s carrying RGB888: 1.944e9 / 24 = 81 Mpixel/s.
CLOCK_CEILING_KHZ = 81_000

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

# --- Branding: how the target's OS names this "monitor" ----------------------
# The base EDID inherits the TC358743 capture adapter's Toshiba identity
# (manufacturer "TSB", name "DCDZ-H2C MOD"). Override both so the target shows
# ESP-KVM instead. The manufacturer is a 3-letter PnP code; the name is <= 13
# characters. Change these two strings to rename the "monitor".
MFR_ID = "ESP"
MONITOR_NAME = "ESP-KVM"


def std_timing(width: int, refresh: int, aspect: str) -> bytes:
    """Encode one standard timing pair (EDID 1.3 section 3.9)."""
    aspect_bits = {"16:10": 0, "4:3": 1, "5:4": 2, "16:9": 3}[aspect]
    return bytes([(width // 8) - 31, (aspect_bits << 6) | (refresh - 60)])


def dtd(clock_khz: int, hact: int, hbl: int, hfp: int, hsw: int,
        vact: int, vbl: int, vfp: int, vsw: int, positive: bool) -> bytes:
    """
    One 18-byte detailed timing descriptor.

    The first one in an EDID is the preferred mode - what a source picks when it
    has no other opinion - so capping a profile means putting the cap here, not
    only trimming the lists. Sizes are the usual packed nibbles; the last byte
    marks digital separate sync with the polarity the mode is defined with.
    """
    px = clock_khz // 10
    flags = 0x18 | (0x02 if positive else 0) | (0x04 if positive else 0)
    return bytes([
        px & 0xFF, px >> 8,
        hact & 0xFF, hbl & 0xFF, ((hact >> 8) << 4) | (hbl >> 8),
        vact & 0xFF, vbl & 0xFF, ((vact >> 8) << 4) | (vbl >> 8),
        hfp & 0xFF, hsw & 0xFF, ((vfp & 0x0F) << 4) | (vsw & 0x0F),
        ((hfp >> 8) << 6) | ((hsw >> 8) << 4) | ((vfp >> 4) << 2) | (vsw >> 4),
        0, 0, 0,   # image size: unstated, this is not a physical panel
        0, 0,      # no borders
        flags,
    ])


# The modes a profile can be capped at, as their standard timings.
DTD_1280x720p60 = dtd(74250, 1280, 370, 110, 40, 720, 30, 5, 5, positive=True)
DTD_1024x768p60 = dtd(65000, 1024, 320, 24, 136, 768, 38, 3, 6, positive=False)


def checksum(block: bytes) -> int:
    return (256 - (sum(block[:127]) & 0xFF)) & 0xFF


def encode_mfr(code: str) -> bytes:
    """Pack a 3-letter PnP manufacturer code into EDID bytes 8-9."""
    code = code.upper()
    assert len(code) == 3 and code.isalpha(), "manufacturer must be 3 letters"
    v = sum((ord(c) - 0x40) << (10 - 5 * i) for i, c in enumerate(code))
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def encode_name(name: str) -> bytes:
    """A 13-byte monitor-name payload: the name, a 0x0A terminator when it is
    shorter than 13, then 0x20 padding."""
    b = bytearray(name.encode("ascii"))
    assert len(b) <= 13, "monitor name is at most 13 characters"
    if len(b) < 13:
        b.append(0x0A)
    while len(b) < 13:
        b.append(0x20)
    return bytes(b[:13])


def brand(edid: bytes) -> bytes:
    """Set the manufacturer ID and the 0xFC monitor-name descriptor, then fix the
    block-0 checksum. Applied to the base so both profiles share the identity."""
    e = bytearray(edid)
    e[8:10] = encode_mfr(MFR_ID)
    for off in (54, 72, 90, 108):  # the four 18-byte descriptor slots in block 0
        if e[off:off + 5] == bytes([0x00, 0x00, 0x00, 0xFC, 0x00]):
            e[off + 5:off + 18] = encode_name(MONITOR_NAME)
            break
    else:
        raise SystemExit("no 0xFC monitor-name descriptor to rename")
    e[127] = checksum(bytes(e[:128]))
    return bytes(e)


# Every profile a target can be offered. The cap is what the profile's preferred
# mode becomes, and what its lists stop at: a KVM often wants the target to stay
# below what the link could carry, because a smaller picture is a faster one.
PROFILES = {
    # name: (preferred DTD, widescreen std timing?, max clock in 10 MHz,
    #        CEA VICs, the cap as (width, height))
    "full": (None, True, 8, [VIC_1920x1080p30 | VIC_NATIVE, VIC_1280x720p60,
                             VIC_720x480p60_16_9, VIC_720x480p60_4_3, VIC_640x480p60],
             (1920, 1080)),
    "720p": (DTD_1280x720p60, True, 8, [VIC_1280x720p60 | VIC_NATIVE,
                                        VIC_720x480p60_16_9, VIC_720x480p60_4_3,
                                        VIC_640x480p60], (1280, 720)),
    "1024x768": (DTD_1024x768p60, False, 7, [VIC_640x480p60 | VIC_NATIVE], (1024, 768)),
}

# An EDID descriptor slot that says "nothing here", for a profile that has no
# second timing to offer. Leaving the base's 1080p descriptor in place would
# advertise the very mode the profile exists to avoid.
DUMMY_DESCRIPTOR = bytes([0x00, 0x00, 0x00, 0x10, 0x00]) + bytes(13)


def dtds_in_extension(edid) -> range:
    """
    Where the CEA extension keeps its detailed timings.

    Byte 130 of the block is the offset to them; everything from there to the
    checksum is timings, then zero padding. This is a second home for modes,
    and forgetting it is how a "capped" profile can go on advertising 1080p -
    which is exactly what the first version of these profiles did.
    """
    return range(128 + edid[130], 256 - 18, 18)


def build_profile(base: bytes, name: str) -> bytes:
    """One EDID, from the known-good base, capped as the profile says."""
    preferred, widescreen, max_clock, vics, cap = PROFILES[name]
    e = bytearray(base)

    # Established timings: what a firmware screen or a bare framebuffer picks.
    # These are all far below every cap, so every profile carries them - and
    # 720x400 is the text mode a BIOS draws its setup on.
    e[35] = EST1_640x480_60 | EST1_800x600_60 | EST1_720x400_70
    e[36] = EST2_1024x768_60 | EST2_800x600_72
    e[37] = 0x00

    # Standard timings: one useful widescreen mode, the rest stay unused (0x0101).
    e[38:40] = std_timing(1280, 60, "16:9") if widescreen else bytes([0x01, 0x01])

    # The two detailed timings. The first is the preferred mode; the base's are
    # both 1920x1080@30, which is right for "full" and wrong for anything capped.
    if preferred is not None:
        e[54:72] = preferred
        e[72:90] = DUMMY_DESCRIPTOR

    # Monitor range descriptor at offset 108: cap the maximum pixel clock (units
    # of 10 MHz) so a source that reads the range instead of the mode list cannot
    # pick something above the cap - or above what the MIPI link can carry.
    assert e[108:113] == bytes([0x00, 0x00, 0x00, 0xFD, 0x00]), "range descriptor moved"
    e[117] = max_clock

    e[127] = checksum(bytes(e[:128]))

    # CEA extension: replace the padded VIC list with the modes we can accept.
    # The video data block starts at 132 (tag 0x40 | length) and is 7 bytes of
    # payload in the base EDID, which is as many as we need.
    assert e[132] == 0x47, "video data block moved or resized"
    # Keep the block length byte honest: tag 2 (video) in the top 3 bits.
    e[132] = 0x40 | len(vics)
    e[133:133 + len(vics)] = bytes(vics)
    # The freed bytes become a zero-length reserved block, which parsers skip.
    for i in range(133 + len(vics), 140):
        e[i] = 0x00

    # The extension's own detailed timings. The base repeats 1920x1080@30 four
    # times here; a capped profile has to replace them, or a source that reads
    # this block - the usual place to find 1080p - is offered the very mode the
    # profile exists to avoid.
    if preferred is not None:
        slots = dtds_in_extension(e)
        e[slots[0]:slots[0] + 18] = preferred
        for off in slots[1:]:
            e[off:off + 18] = bytes(18)  # zero padding: no more timings here

    e[255] = checksum(bytes(e[128:256]))
    return bytes(e)


def cap_range(edid: bytes, max_clock: int) -> bytes:
    """
    Lower the range descriptor's maximum pixel clock (units of 10 MHz).

    The base EDID inherited 160 MHz from the capture adapter it came from -
    twice what these two MIPI lanes can carry. It advertises no mode that fast,
    so nothing has gone wrong in practice, but a source that reads the range
    rather than the mode list is within its rights to try one.
    """
    e = bytearray(edid)
    assert e[108:113] == bytes([0x00, 0x00, 0x00, 0xFD, 0x00]), "range descriptor moved"
    e[117] = max_clock
    e[127] = checksum(bytes(e[:128]))
    return bytes(e)


def read_dtd(edid: bytes, off: int):
    """(clock kHz, width, height) of the timing at @p off, or None if unused."""
    clock = (edid[off] | edid[off + 1] << 8) * 10
    if clock == 0:
        return None
    return (clock,
            edid[off + 2] | ((edid[off + 4] >> 4) << 8),
            edid[off + 5] | ((edid[off + 7] >> 4) << 8))


def audit(name: str, edid: bytes) -> None:
    """
    Refuse to emit an EDID that breaks the rules that turn a screen black: a
    wrong checksum, a mode the link cannot carry, or a mode above the profile's
    own cap. Every detailed timing is checked, in both blocks - a capped profile
    that still advertises 1080p in the extension is a cap in name only, and the
    range descriptor and the timings must not contradict each other.
    """
    if len(edid) != 256:
        raise SystemExit(f"{name}: EDID must be 256 bytes, got {len(edid)}")
    if checksum(edid[:128]) != edid[127] or checksum(edid[128:]) != edid[255]:
        raise SystemExit(f"{name}: checksum is wrong")

    cap_w, cap_h = PROFILES.get(name, (None, None, None, None, (1920, 1080)))[4]
    allowed_khz = min(CLOCK_CEILING_KHZ, edid[117] * 10_000)

    for off in list((54, 72)) + list(dtds_in_extension(edid)):
        timing = read_dtd(edid, off)
        if timing is None:
            continue
        clock, w, h = timing
        where = "block 0" if off < 128 else "the extension"
        if clock > CLOCK_CEILING_KHZ:
            raise SystemExit(f"{name}: the timing at {off} ({where}) is {clock} kHz, "
                             f"above the {CLOCK_CEILING_KHZ} kHz the link carries")
        if clock > allowed_khz:
            raise SystemExit(f"{name}: the timing at {off} ({where}) is {clock} kHz, "
                             f"above the {allowed_khz} kHz this profile's range allows")
        if w > cap_w or h > cap_h:
            raise SystemExit(f"{name}: the timing at {off} ({where}) is {w}x{h}, "
                             f"above the profile's cap of {cap_w}x{cap_h}")
    if edid[117] * 10_000 > CLOCK_CEILING_KHZ:
        raise SystemExit(f"{name}: the range descriptor allows more than the link carries")


def c_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[TC358743_EDID_TOTAL_LEN] = {{"]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


EST1_MODES = [(0x80, "720x400@70"), (0x20, "640x480@60"), (0x01, "800x600@60")]
EST2_MODES = [(0x80, "800x600@72"), (0x08, "1024x768@60")]
VIC_NAMES = {1: "640x480@60", 2: "720x480@60 4:3", 3: "720x480@60 16:9",
             4: "1280x720@60", 34: "1920x1080@30"}


def describe(name: str, edid: bytes) -> str:
    """
    Read a finished profile back and say what it offers.

    Decoded from the bytes rather than from the inputs, so this catches a field
    written to the wrong offset - which is the mistake this file is prone to.
    """
    out = [f"{name}:"]
    est = [n for bit, n in EST1_MODES if edid[35] & bit]
    est += [n for bit, n in EST2_MODES if edid[36] & bit]
    out.append("  established: " + (", ".join(est) or "none"))

    std = []
    for off in range(38, 54, 2):
        if edid[off] == 0x01 and edid[off + 1] == 0x01:
            continue
        width = (edid[off] + 31) * 8
        std.append(f"{width}x? @{(edid[off + 1] & 0x3F) + 60}")
    out.append("  standard:    " + (", ".join(std) or "none"))

    for off in (54, 72):
        timing = read_dtd(edid, off)
        label = "preferred" if off == 54 else "second   "
        if timing is None:
            out.append(f"  {label}:   unused")
            continue
        clock, w, h = timing
        out.append(f"  {label}:   {w}x{h}, {clock / 1000:g} MHz")

    ext = [read_dtd(edid, off) for off in dtds_in_extension(edid)]
    shown = [f"{w}x{h} @{c / 1000:g} MHz" for c, w, h in (x for x in ext if x)]
    out.append("  extension:   " + (", ".join(shown) or "no timings"))

    n = edid[132] & 0x1F
    vics = [VIC_NAMES.get(b & 0x7F, f"VIC {b & 0x7F}") + (" (native)" if b & 0x80 else "")
            for b in edid[133:133 + n]]
    out.append("  over HDMI:   " + ", ".join(vics))
    out.append(f"  max clock:   {edid[117] * 10} MHz")
    return "\n".join(out)


def main() -> int:
    base = brand(BASE)
    profiles = {
        "full": build_profile(base, "full"),
        "1080p30": cap_range(base, 8),  # the original single-mode EDID
        "720p": build_profile(base, "720p"),
        "1024x768": build_profile(base, "1024x768"),
    }
    for name, edid in profiles.items():
        audit(name, edid)

    if "--dump" in sys.argv:
        for name, edid in profiles.items():
            print(describe(name, edid))
        return 0

    print("/*")
    print(" * SPDX-FileCopyrightText: 2026 ESP-KVM contributors")
    print(" * SPDX-License-Identifier: Apache-2.0")
    print(" *")
    print(" * Generated by tools/gen_edid.py - do not edit by hand.")
    print(" *")
    print(" * What each profile tells the target it can display:")
    print(" *   full      640x480, 720x400, 800x600, 1024x768, 1280x720, 1920x1080@30")
    print(" *   1080p30   the original single-mode EDID, for sources that reject a list")
    print(" *   720p      the same list without 1080p, and 1280x720 preferred")
    print(" *   1024x768  everything up to 1024x768, which is also preferred")
    print(" *")
    print(" * The capped ones are not about what the link can carry - they are about")
    print(" * what the target should send. A smaller picture encodes faster, and on")
    print(" * pre-3.0 silicon that is the difference between 7 fps and something a")
    print(" * person can work in.")
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
    print(c_array("tc358743_edid_full", profiles["full"]))
    print()
    print(c_array("tc358743_edid_1080p30", profiles["1080p30"]))
    print()
    print(c_array("tc358743_edid_720p", profiles["720p"]))
    print()
    print(c_array("tc358743_edid_1024x768", profiles["1024x768"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
