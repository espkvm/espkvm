# If it does not work

Six things account for most of it, and each has a way to tell it apart from
the others without a multimeter.

## The target never sees a keyboard or a mouse

Check which connector the cable
is in. On the Waveshare ESP32-P4-ETH the USB-C socket is only the serial bridge,
for flashing and the log. The port that presents the keyboard and mouse is the
small 4-pin **MX1.25 header** next to it, marked `USB` on the silkscreen. No
cable from that USB-C will ever reach the target. The other boards put the same
port on USB-C or USB-A - see [Cables](BOARDS.md#cables).

To check it from the device instead of by eye, open
`https://espkvm.local/api/v1/system/usbprobe`. An empty trace means the target
has never seen the KVM at all, which is exactly what a cable in the serial port
looks like.

## The keyboard does not work, or the picture never starts

Both are the same cause, seen from two sides: the keyboard, the
mouse and H.264 all ride a WebSocket, and a browser only opens one on a page it
considers secure - which a self-signed certificate is not, until it is trusted.
MJPEG and signing in work anyway, which is why the rest looks fine.

One step tells it apart from a capture fault: set **Settings -> Video -> Stream
codec** to `mjpeg`, which is plain HTTP. If the picture appears, it is the
certificate. Install the device's CA and reach it by name - [SETUP.md](SETUP.md#https-and-the-devices-certificate) says how.

## No signal, or a picture only after the target has booted

The KVM
holds HDMI hotplug low until it has loaded its EDID and started capture, and that
takes a good ten seconds from power on. A machine that looks for a monitor during
its own POST, and finds none, may decide it has no external screen and never turn
the output on. So power the KVM first, or replug HDMI once the KVM is up. If the
picture arrives but the source refuses a mode, try a capped **EDID profile** in
Settings -> Video.

While a source is powered and sending nothing, the device offers it a fresh
hotplug by itself - ten seconds in, then twenty, then forty. That is all one end
of a cable can do, and some machines never look at the connection again once
they have started: an Orange Pi 4 Pro reported here would not come back after a
re-plug into this device, and would not come back for a television either.
The fix for that one is on the machine, and somebody has written it up - a udev
rule that re-applies the mode when the connector changes state:
[check456-beep/orangepi-hdmi-fix](https://github.com/check456-beep/orangepi-hdmi-fix).

## A VGA, DVI or DisplayPort target

Many servers have only VGA. An active VGA-to-HDMI converter works, but the target talks to the converter, not to the KVM. It reads the converter's EDID instead of the one the device offers, so the device's mode list and the EDID profiles in Settings do nothing. The target may pick a mode the capture cannot take: no picture, or a "too fast" note in the console.

Set the mode on the target yourself. 1280x720 or 1024x768 at 60 Hz, or 1080p at 30 Hz, always fit. On Linux with X11, xrandr can add a mode the target does not list:

```
xrandr --newmode "1280x720_60.00" 74.50 1280 1344 1472 1664 720 723 728 748 -hsync +vsync
xrandr --addmode DP-2 "1280x720_60.00"
xrandr --output DP-2 --mode "1280x720_60.00"
```

Use the output name `xrandr` prints for that port, and run it on the target's own screen, not over SSH. Put the lines in `.profile` to keep them. A BIOS screen stays at whatever the converter takes, which is usually fine.

A passive DVI-to-HDMI adapter or cable is only a different plug: the target sees the device's EDID, and nothing changes. The same goes for a DisplayPort port marked DP++ with a passive adapter. An active DisplayPort-to-HDMI adapter behaves like the VGA converter above.

## The HDMI signal is there, but no picture

The log says `csi frame wait timeout`: the capture board sees the source, and nothing arrives over the ribbon. The same line says what the P4's receiver sees on the ribbon's lanes, and `GET /api/v1/video/status` has it too, as `csi`, sampled about once a second.

- `stop d0=1 d1=1 clk=1` (`"stop":[1,1],"stopClk":1`) is a lane held in its idle state by the bridge. A working link reads this between frames, so the cable carries the lanes. Look at the bridge's settings instead: the mode, the colour format.
- `stop d0=0 d1=0 clk=0` with `clk_hs=0` means nothing holds the lanes at all. The bridge talks over I2C on the same cable, so the lanes do not reach the P4: a capture board that wires its connector differently, a cable of the wrong kind, a connector that does not close on the ribbon.
- `clk_hs=1` is the clock lane running: the bridge sends.
- `phy_err` other than 0 means the lanes carry something the receiver cannot read, often a long or damaged cable.

## A pin you picked in Settings does nothing

It may not be a pin at all. The P4
has 55 GPIOs and a board brings out maybe half. **Settings -> Pins** draws the
board's expansion header the way it is printed, with what holds each pin, so you
can find a free one where the wire actually goes. Some pins are also taken by
hardware the firmware never touches: on the Function EV, GPIO 45 carries the
microSD power net whether we use it or not.

## It rebooted on its own

The log line at the top of every boot says how the
last run ended - a panic, a watchdog, a brownout or a plain reset are four
different bugs. If it was a panic, the firmware also wrote a **crash dump** to
flash before rebooting: registers and task stacks as they were. **Diagnostics ->
Download the crash dump** hands it over as a file, and attaching it to an issue
turns "it rebooted overnight" into a backtrace. It carries no settings and no
keys. A device that was updated over the network keeps the partition table it
was first flashed with, so the dump needs one cable flash of the table to work;
without it the device simply never has one.

Flashing problems - the port not appearing, drivers, permissions - are in
[FLASHING.md](FLASHING.md). Anything else: the device keeps its own log
across a restart, and **Diagnostics -> Download the log** is the fastest way to
show what happened.

## Still stuck?

Ask in [Discussions](https://github.com/orgs/espkvm/discussions) -
say which board, which capture board and what the target is, and attach the log
from Diagnostics in the console (the button with the chip temperature). If it is
clearly a bug, [open an issue](https://github.com/espkvm/espkvm/issues/new) instead.
