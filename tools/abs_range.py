#!/usr/bin/env python3
"""Print the absolute axis ranges an evdev device reports.

    sudo python3 tools/abs_range.py /dev/input/event8

Reading an event device needs root, and this reads only - it sends nothing to
the device and generates no input. Use it to confirm the host agrees with the
descriptor: an absolute pointer that enumerated correctly reports 0..32767 on
both axes, matching USB_HID_ABS_MAX in components/kvm_hid/include/usb_hid.h.
"""

import fcntl
import struct
import sys

# EVIOCGABS(axis): read a 24-byte struct input_absinfo
_ABS_STRUCT = "iiiiii"


def eviocgabs(axis: int) -> int:
    size = struct.calcsize(_ABS_STRUCT)
    return 0x80000000 | (size << 16) | (ord("E") << 8) | (0x40 + axis)


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "/dev/input/event0"
    try:
        dev = open(path, "rb")
    except PermissionError:
        print(f"{path}: permission denied - run with sudo", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"{path}: {exc}", file=sys.stderr)
        return 1

    with dev:
        for axis, name in ((0, "ABS_X"), (1, "ABS_Y")):
            buf = bytearray(struct.calcsize(_ABS_STRUCT))
            try:
                fcntl.ioctl(dev, eviocgabs(axis), buf)
            except OSError as exc:
                print(f"{name}: not present ({exc})")
                continue
            value, minimum, maximum, fuzz, flat, resolution = struct.unpack(_ABS_STRUCT, buf)
            print(f"{name}: min={minimum} max={maximum} current={value} "
                  f"fuzz={fuzz} flat={flat} res={resolution}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
