#!/usr/bin/env python3
"""Exercise the ESP-KVM control channel without a browser.

    python3 tools/hid_probe.py 10.42.0.151 status
    python3 tools/hid_probe.py 10.42.0.151 corners
    python3 tools/hid_probe.py 10.42.0.151 key 0x28        # Enter
    python3 tools/hid_probe.py 10.42.0.151 abs 16383 16383 # centre of the screen

A browser makes a poor test instrument for HID: pointer lock, focus and the
page's own coalescing all sit between the intent and the wire. This speaks
protocol v2 directly, so a failure is unambiguously in the firmware or the
descriptor.

Protocol v2 is defined in components/kvm_web/http_server.c.
"""

import asyncio
import sys

import websockets

MSG_MOUSE_ABS = 0x01
MSG_MOUSE_REL = 0x02
MSG_KEYBOARD = 0x03
MSG_CONSUMER = 0x04
MSG_RELEASE_ALL = 0x05
MSG_PING = 0x06
MSG_STATUS = 0x81
MSG_PONG = 0x82

ABS_MAX = 32767

LED_NAMES = [(0x01, "Num"), (0x02, "Caps"), (0x04, "Scroll")]


def abs_frame(x: int, y: int, buttons: int = 0, wheel: int = 0, pan: int = 0) -> bytes:
    return bytes([MSG_MOUSE_ABS, buttons,
                  x & 0xFF, (x >> 8) & 0xFF,
                  y & 0xFF, (y >> 8) & 0xFF,
                  wheel & 0xFF, pan & 0xFF])


def rel_frame(dx: int, dy: int, buttons: int = 0, wheel: int = 0, pan: int = 0) -> bytes:
    return bytes([MSG_MOUSE_REL, buttons,
                  dx & 0xFF, (dx >> 8) & 0xFF,
                  dy & 0xFF, (dy >> 8) & 0xFF,
                  wheel & 0xFF, pan & 0xFF])


def key_frame(modifier: int = 0, keys=()) -> bytes:
    k = list(keys)[:6] + [0] * (6 - len(list(keys)[:6]))
    return bytes([MSG_KEYBOARD, modifier] + k)


def describe_status(payload: bytes) -> str:
    attached = "attached" if payload[1] & 1 else "no target"
    leds = payload[2]
    lit = [name for bit, name in LED_NAMES if leds & bit] or ["none"]
    return f"target {attached}, leds {leds:#04x} ({', '.join(lit)})"


async def run(host: str, argv: list[str]) -> int:
    uri = f"ws://{host}/ws"
    async with websockets.connect(uri, max_size=None) as ws:
        cmd = argv[0] if argv else "status"

        async def drain(seconds: float) -> None:
            """Print whatever the device pushes for a while."""
            try:
                while True:
                    msg = await asyncio.wait_for(ws.recv(), timeout=seconds)
                    if isinstance(msg, bytes) and msg and msg[0] == MSG_STATUS and len(msg) >= 3:
                        print("  <-", describe_status(msg))
                    elif isinstance(msg, bytes) and msg and msg[0] == MSG_PONG:
                        print("  <- pong")
                    else:
                        print("  <-", msg)
            except asyncio.TimeoutError:
                pass

        if cmd == "status":
            await ws.send(bytes([MSG_PING]))
            await drain(2.0)

        elif cmd == "corners":
            # Walks the four corners then returns to the middle. With a working
            # absolute descriptor the target's cursor lands exactly on each
            # corner; with a relative one it drifts and never reaches them.
            for name, (x, y) in [
                ("top-left", (0, 0)),
                ("top-right", (ABS_MAX, 0)),
                ("bottom-right", (ABS_MAX, ABS_MAX)),
                ("bottom-left", (0, ABS_MAX)),
                ("centre", (ABS_MAX // 2, ABS_MAX // 2)),
            ]:
                print(f"  -> {name} ({x}, {y})")
                await ws.send(abs_frame(x, y))
                await asyncio.sleep(0.7)
            await drain(0.3)

        elif cmd == "abs":
            x, y = int(argv[1]), int(argv[2])
            await ws.send(abs_frame(x, y))
            await drain(0.3)

        elif cmd == "rel":
            dx, dy = int(argv[1]), int(argv[2])
            await ws.send(rel_frame(dx, dy))
            await drain(0.3)

        elif cmd == "key":
            usage = int(argv[1], 0)
            modifier = int(argv[2], 0) if len(argv) > 2 else 0
            await ws.send(key_frame(modifier, [usage]))
            await asyncio.sleep(0.05)
            await ws.send(key_frame(0, []))
            await drain(0.5)

        elif cmd == "release":
            await ws.send(bytes([MSG_RELEASE_ALL]))
            await drain(0.3)

        elif cmd == "watch":
            print("  watching for status pushes, Ctrl-C to stop")
            await drain(3600.0)

        else:
            print(f"unknown command {cmd!r}", file=sys.stderr)
            return 2
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    return asyncio.run(run(sys.argv[1], sys.argv[2:]))


if __name__ == "__main__":
    raise SystemExit(main())
