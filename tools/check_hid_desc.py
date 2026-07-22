#!/usr/bin/env python3
"""Parse the HID report descriptors out of a built firmware and check them.

    . tools/env.sh && python3 tools/check_hid_desc.py

A malformed report descriptor is one of the least pleasant bugs in USB work:
the target simply ignores the device, or accepts it and then behaves oddly, and
nothing in the firmware log says why. This reads the descriptors straight out
of the ELF, decodes them, and reports the two things that are easy to get wrong
by hand - unbalanced collections and a report length that disagrees with the C
struct being sent.
"""

import pathlib
import re
import subprocess
import sys

ELF = pathlib.Path("build/espkvm.elf")
RODATA_SECTION = ".flash.rodata"

# Descriptor symbol -> the struct sizes we expect per report ID, so a change to
# one without the other is caught here rather than on the target's desk.
EXPECTED = {
    "s_kbd_report_desc": {0: 8},          # boot keyboard input report
    "s_pointer_report_desc": {1: 7, 2: 7, 3: 2},  # abs mouse, rel mouse, consumer
}

ITEM_NAMES = {
    0x04: "Usage Page", 0x08: "Usage", 0x14: "Logical Min", 0x24: "Logical Max",
    0x34: "Physical Min", 0x44: "Physical Max", 0x74: "Report Size",
    0x84: "Report ID", 0x94: "Report Count", 0x80: "Input", 0x90: "Output",
    0xA0: "Collection", 0xC0: "End Collection", 0x18: "Usage Min", 0x28: "Usage Max",
}

TAG_INPUT = 0x80
TAG_COLLECTION = 0xA0
TAG_END_COLLECTION = 0xC0
TAG_REPORT_ID = 0x84
TAG_REPORT_SIZE = 0x74
TAG_REPORT_COUNT = 0x94


def run(*args: str) -> str:
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def toolchain(tool: str) -> str:
    return f"riscv32-esp-elf-{tool}"


def symbols() -> dict[str, tuple[int, int]]:
    out = run(toolchain("nm"), "-S", str(ELF))
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in EXPECTED:
            found[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return found


def rodata() -> tuple[bytes, int]:
    headers = run(toolchain("objdump"), "-h", str(ELF))
    m = re.search(rf"{re.escape(RODATA_SECTION)}\s+\S+\s+(\S+)", headers)
    if not m:
        raise SystemExit(f"{RODATA_SECTION} not found in {ELF}")
    base = int(m.group(1), 16)
    blob = subprocess.run(
        [toolchain("objcopy"), "-O", "binary", f"--only-section={RODATA_SECTION}",
         str(ELF), "/dev/stdout"],
        capture_output=True, check=True).stdout
    return blob, base


def decode(blob: bytes, verbose: bool) -> tuple[dict[int, int], list[str]]:
    """@return input report sizes in bytes per report ID, and any problems."""
    i = 0
    depth = 0
    problems: list[str] = []
    bits: dict[int, int] = {}
    report_id = 0
    size_bits = 0
    count = 0

    while i < len(blob):
        prefix = blob[i]
        i += 1
        length = prefix & 0x03
        length = 4 if length == 3 else length
        tag = prefix & 0xFC
        if i + length > len(blob):
            problems.append(f"item at offset {i - 1} runs past the end of the descriptor")
            break
        value = int.from_bytes(blob[i:i + length], "little") if length else 0
        i += length

        if tag == TAG_COLLECTION:
            depth += 1
        elif tag == TAG_END_COLLECTION:
            depth -= 1
            if depth < 0:
                problems.append("End Collection without a matching Collection")
        elif tag == TAG_REPORT_ID:
            report_id = value
        elif tag == TAG_REPORT_SIZE:
            size_bits = value
        elif tag == TAG_REPORT_COUNT:
            count = value
        elif tag == TAG_INPUT:
            bits[report_id] = bits.get(report_id, 0) + size_bits * count

        if verbose:
            print(f"    {'  ' * max(depth - (1 if tag == TAG_COLLECTION else 0), 0)}"
                  f"{ITEM_NAMES.get(tag, f'tag {tag:#04x}')}"
                  f"{f' = {value:#x}' if length else ''}")

    if depth != 0:
        problems.append(f"collections are unbalanced (depth {depth} at the end)")

    sizes = {}
    for rid, n in bits.items():
        if n % 8:
            problems.append(f"report {rid} is {n} bits, which is not a whole number of bytes")
        sizes[rid] = n // 8
    return sizes, problems


def main() -> int:
    verbose = "-v" in sys.argv
    if not ELF.exists():
        print(f"{ELF} not found - build first", file=sys.stderr)
        return 2

    blob, base = rodata()
    syms = symbols()
    missing = set(EXPECTED) - set(syms)
    if missing:
        print(f"symbols not found in {ELF}: {', '.join(sorted(missing))}", file=sys.stderr)
        return 2

    failures = 0
    for name, expected in EXPECTED.items():
        addr, size = syms[name]
        desc = blob[addr - base:addr - base + size]
        print(f"{name}: {size} bytes")
        sizes, problems = decode(desc, verbose)

        for rid, want in sorted(expected.items()):
            got = sizes.get(rid)
            if got is None:
                problems.append(f"no input report with ID {rid}")
            elif got != want:
                problems.append(f"report {rid} is {got} bytes, the sender writes {want}")
        extra = set(sizes) - set(expected)
        if extra:
            problems.append(f"undeclared report IDs present: {sorted(extra)}")

        if problems:
            failures += 1
            for p in problems:
                print(f"  FAIL {p}")
        else:
            detail = ", ".join(f"id {r}: {s} B" for r, s in sorted(sizes.items()))
            print(f"  ok   {detail}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
