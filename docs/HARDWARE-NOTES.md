# Hardware notes

What this board actually does, as opposed to what its documentation says.

Every number here was measured on the hardware in front of us, and every entry
under "things that turned out not to be true" cost real time to discover. The
stage-by-stage plan and the outstanding work are tracked outside the
repository.

## The hardware in front of us

| | |
|---|---|
| Board | Waveshare ESP32-P4-ETH |
| Chip | ESP32-P4 **rev v1.3**, 360 MHz dual core |
| PSRAM / flash | 32 MB @ 200 MHz / **32 MB present**, 16 MB configured |
| Capture | Geekworm C790, TC358743 HDMI -> MIPI CSI-2, 2 lanes @ 972 Mbit/s |
| USB | one USB-C: the CH343 flashing/console bridge (`/dev/ttyACM0`). The USB 2.0 OTG HS that presents the keyboard/mouse to the target is on the **MX1.25 connector**, not a USB port |
| OTG power | the target's 5 V comes back down that OTG lead, so **pulling it at the target's end reboots the device**. Worth knowing before telling somebody to re-plug the cable to fix a keyboard: it takes the KVM with it |
| Network | NetworkManager profile `espkvm-link` shares `enp0s31f6`; device lands on **10.42.0.151** |
| Toolchain | ESP-IDF 6.1 in `~/esp/esp-idf`; cmake and ninja live in the IDF python env, not the distro |

Build: `. tools/env.sh && idf.py -p /dev/ttyACM0 -b 921600 flash`

## Measured on this hardware

MJPEG, quality 70, published frames only:

| Mode | fps | Bitrate |
|---|---|---|
| 1920x1080 | 20 | 17.5 Mbit/s |
| 1280x720 | 45.5 | 16.8 Mbit/s |
| 1024x768 | 56 | 17.6 Mbit/s |

With unchanged frames skipped: a still screen costs **0 kbit/s**, a moving
cursor at 1080p costs 8.5 Mbit/s at 13 fps.

H.264 path (`CSI RGB888 -> PPA -> YUV420 -> encoder`), both stages overlapped:

| Size | PPA | Encode | Sum |
|---|---|---|---|
| 1920x1080 | 76.3 ms | 30.9 ms | 107 ms |
| 1280x720 | 33.6 ms | 13.7 ms | 47 ms |
| 960x540 | 19.1 ms | 7.9 ms | 27 ms |
| 640x480 | 11.4 ms | 4.7 ms | 16 ms |

Cost is linear in pixel count - the PPA is bandwidth-bound. Those figures come
from a probe running on an otherwise idle chip. **With the capture pipeline
actually running, one 1080p frame costs 145 ms**, because the CSI DMA is
writing 6 MB per frame into the same PSRAM the PPA is reading. The stream
settles at 5-7 fps. At 1280x720 the same contention shows as a PPA pass of
57-60 ms against the idle probe's 33.6 ms, and the stream settles at 17 fps -
the encode, 24 ms, runs underneath it and costs nothing extra. That is the
ceiling on this silicon: the PPA pass is the frame period.

So the trade at 1080p is 20 fps of MJPEG against 6 fps of H.264 - and, on an
idle desktop, 8.5 Mbit/s against **170 kbit/s**. H.264 is for links too narrow
to carry MJPEG at all, not for smoothness.

**That last line is about rev v1.3 silicon only.** On rev 3.x the capture hands
the encoder YUV422 directly, the PPA step goes away, and 1080p H.264 runs at
22-24 fps (28 at 720p) - see the frame-rate table in the README. Numbers quoted
from this page have been read as the project's H.264 speed more than once; they
are the speed of the board at the top of it.

Verified end to end by capturing the Annex-B stream off the WebSocket and
decoding it with ffmpeg: `Constrained Baseline, 1920x1080, yuv420p`, correct
colours, no stride or cropping artefacts (the encoder pads 1080 to 1088 and
crops it back in the SPS).

TLS, measured on this board:

| | |
|---|---|
| Certificate generation (ECDSA P-256, first boot) | **20 ms** |
| Full handshake, per connection | **~330 ms** |
| Firmware upload (1 MB) over HTTPS | works, no tuning needed |
| Idle cost of the TLS listener | ~15 KB of internal RAM |

RSA would have been minutes rather than milliseconds; the P4's ECC accelerator
is what makes generating on-device reasonable. The handshake is the part the
operator feels - a console opens several connections - so session tickets are
enabled.

## A sleeping target can take its port's power with it

Seen on the desktop this device is wired to, 2026-09-03. The machine slept; the
keyboard and mouse were dead when it woke, and stayed dead through three
re-plugs from the console, a device restart, and a cable pull at the target's
end. What fixed it was pulling every cable and giving the board a cold start.

`tud_connected()` is what settles it - there was **no live bus at all**, so
every re-plug was toggling a pull-up on a wire with nobody on the other end. The
target had suspended the bus on its way down (the log says so now) and its port
came back from sleep without power. Nothing on this end can reach that: the port
has to be re-powered, by re-plugging at the target or restarting it.

The lesson for the firmware was the diagnosis, not a fix: the device knew the
difference all along and did not say it. It says it now, in the console and in
`GET /api/v1/system/usbprobe`.

## M5Stack Unit PoE-P4 and its LT6911D add-on

The Add-on Display In carries an LT6911D, and its reset line - CAM_RST on
GPIO 18, the same pin the TC358743 boards use - works the other way round from
the TC358743's RESETN. Drive it high, the way a TC358743 wants to be let go, and
the whole capture I2C bus reads empty: not one address answers anywhere between
0x08 and 0x77. Hold it low, or leave the pin floating, and the bridge is there at
0x2b within half a second - which is 0x56, the address M5Stack publishes, written
as an 8-bit one.

Measured on a plain Unit PoE-P4 (chip rev v1.3) with the add-on fitted and
powered over its USB-C, 2026-09-19, by scanning the bus under all three states of
the pin. The ribbon is seated right, then: the same connector carries the I2C the
bridge answers on, and a shifted or flipped one would take that with it. `CONFIG_KVM_BRIDGE_RST_ACTIVE_HIGH=y` in `boards/m5_poe_p4.defaults` is
the fix. Everything else the scan ruled out first: the pins are M5Stack's own
GPIO 0/1 and swapping them changes nothing, the bus has external pull-ups on both
lines, the add-on carries its own 24 MHz crystal so nothing is owed on CAM_MCLK,
and the bridge is no slower than its reset line - it answers on the first probe.

**And it gives a picture.** What it says about itself, through Lontium's
bank-switched register space (write 0xe0 to 0xff, 0x01 to 0xee, 0xe1 to 0xff,
then read 0x00 and 0x01): **chip id 0x2102**. A PC on the other end of the HDMI
cable sees a monitor and offers 1920x1080, 1280x720, 720x480 and 640x480, so the
chip's own firmware runs, holds the EDID and locks to the source without this
firmware saying a word to it - and it raises its lanes by itself too. Measured at
**23 fps at 1280x720**, MJPEG, on 2026-09-19.

Getting there cost a day, because two mistakes of our own each looked exactly
like a bridge that does not transmit.

**Close the register bus after every access.** 0xee in bank 0xe0 opens the
chip's internal registers to I2C. Lontium's own Linux drivers open it for one
access and close it again; this firmware opened it and left it open, and with it
open the bridge's firmware is locked out of its own registers - the HDMI side
goes down, a PC that saw a monitor loses it, and the CSI receiver counts zero DMA
completions for ever. The chip comes back when the add-on is power-cycled.
Driving GPIO 18 was blamed for this first, and is innocent: a build that never
touched the pin behaved the same.

**Read the CSI host, not just the bridge, when no frames arrive.**
`MIPI_CSI_HOST.phy_rx.phy_rxclkactivehs` is 1 whenever the source drives the
clock lane in high speed. That one bit separates "nothing on the lanes" from
"data we are decoding wrong", and it said 1 through every run that looked dead.
It is logged by `capture_debug_csi_timeout` behind `CONFIG_KVM_TC358743_ADV_DEBUG`
and is the first thing to read on any board that captures nothing.

**It loses the mode when the source changes one, and only its reset pin gets it
back (2026-09-20).** Switching an Ubuntu target to a text console left the chip
answering I2C, reporting a pixel clock, and returning all zeros for the timings
- for ten minutes, and through the target going back to the desktop. Pulling
CAM_RST recovers it. Two things that matters for. The chip needs about two and
a half seconds with its register bus to itself after a reset, or the mode reads
stop it re-locking: the same reset at start-up works because nothing polls yet.
And 0x80 is not a stand-in for DDC5V. It is not a pixel clock at all, whatever
the Linux drivers call it: 0x80..0x83 read 25 05 13 01 always - with a mode, with
none, with the screen asleep, with the cable out - which looks like a firmware
date, 2025-05-13 (checked 2026-09-22). The "37 MHz" was 0x25. So nothing this
chip is known to report tells a sleeping source from a wedged one, and the
refresh rate is unknown on this board. The driver dumps
bank 0xe0 0x80..0x9f on each loss so the two can be compared.

**The JPEG engine cannot take the LT6911D's byte order, and the header trick
that looked like it could does not survive a real decoder (tried 2026-09-23).**
`pixel_reverse` in the encode config gets the luma, the geometry and the
sharpness right and leaves Cb and Cr exchanged, which would remove the PPA pass
and take 1080p MJPEG from 10 to 24 fps. Exchanging the second and third
component numbers in the JPEG's SOF0 does swap the channels back, and lenient
viewers show a perfect picture - but libjpeg and the browser refuse the file
outright ("broken data stream"), so the console shows nothing at all. Moving the
swap to the SOS component selectors is refused the same way. The 2D-DMA's
scramble, which would do it in the encoder's own input path, is documented for
3-byte pixels only. So the PPA pass stays: 73 ms a frame at 1080p, and the
measurements below are what it buys.

**The LT6911 sends YUV422, not RGB888.** The CSI bridge's data-type filter was
set to 0x24 and the wire carries 0x1e, so every packet was discarded. Set the
filter right and the frames arrive.

**And the bytes arrive in the reverse of the order the JPEG engine reads.** The
bridge sends Y U Y V. The engine takes the two luma of a group from its second
and fourth byte and emits them in the reverse of that order, and the chroma from
the first and third - so what it wants is those same four bytes reversed. That
was measured against a screenshot taken on the source machine, not read out of a
manual: Y U Y V comes out flat green, U Y V Y comes out with the right luma but
every pair of columns exchanged and red and blue swapped, and the reversal puts
every pixel exactly where the source had it (mean difference 1.4 grey levels,
against 6.6 for the version before it).

Nothing else in the chain can do that reversal. The CSI bridge's endian bit
reverses a whole 64-bit word - the field is two bits wide but has only those two
states. The JPEG encoder's own `pixel_reverse` swaps the two bytes of a 16-bit
one. The bridge's colour-mode block, which would simply be told the order, does
not exist below rev 3.0. The PPA can: call each 4:2:2 pair one ARGB8888 pixel,
which it is, and ask for the RGB swap, which reverses the four bytes. One pass,
no scaling, nothing lost off the edge.
`components/video_pipeline/capture_yuv_swap.c`.

**H.264 works here too, up to 1280x720.** The encoder below rev 3.0 takes one
layout only - YUV420 with a chroma byte in front of every two luma - and every
block that could make it from YUV422 is revision-gated, the PPA's YUV inputs
included. But the rearrangement needs no arithmetic: three of every four bytes
are copied and the fourth, the odd row's chroma, is dropped. So the CPU does it,
in place of the PPA pass MJPEG pays rather than on top of it.

Three things about it were each found the hard way.

**The triple is the reverse of what the PPA writes.** Reading the PPA's own
YUV420 output says chroma first, then the two luma in natural order, U on the
even rows. Feeding the encoder exactly that gives red for blue and every pair of
columns exchanged; reversing the triple - the other chroma, the two luma the
other way round - is what looks right. The same habit as the JPEG engine, which
also reads its group backwards. What the PPA writes and what the encoder reads
are not the same order, and only the second one matters.

**It is bounded by memory, not by the processor.** Rewriting the loop to move
whole words instead of bytes changed nothing, which puts it at about 84 MB/s,
PSRAM's own speed for this pattern: 12.8 ms at 640x480, 37 ms at 720p, 86 ms at
1080p.

**And it must hand the core back.** Every other stage in this pipeline waits on
hardware, and that wait is what lets the idle task run. This one does not, and at
37 ms a frame against 33 ms between them the capture task stops blocking
altogether: the idle task is never scheduled and the task watchdog reboots the
device five seconds later. It cost two reboots to see. One `vTaskDelay(1)` a
frame fixes it, at the price of a tick - 15 fps rather than 18.

1080p runs too, at 6 fps, and whether it starts at all depends on memory rather
than on speed: the encoder's reference frame wants one contiguous block, and this
board's YUV422 path already holds a 6.2 MB capture ring and a 4 MB reordering
buffer. On a fresh boot the block is there; after the heap has been worked it may
not be, and the firmware says so and stays on MJPEG - the same intermittent
shortage the other pre-3.0 boards have. Handing the reordering buffer back while
H.264 runs does free the block reliably, and then MJPEG cannot get it again when
it returns and serves a green picture, so the buffer is taken once and kept.

Measured: 15 fps at 1280x720 against MJPEG's 24, and 6 fps at 1080p, each for
roughly a third of MJPEG's bandwidth.

**The memory budget on this board is the tightest in the project, and getting it
wrong takes the picture away entirely.** Three things want contiguous PSRAM at
once: the capture ring, the reordering buffer that every JPEG goes through, and
the H.264 encoder. Two lessons, both paid for:

The ring must be sized for the format actually captured. `CAPTURE_MAX_PIXEL_BYTES`
keyed off the revision, so this board allocated three bytes a pixel for a
two-byte format - 4.1 MB held from boot for nothing, and it showed up as the JPEG
encoder failing to get its own 2.4 MB buffers, which leaves no codec running at
all. Free PSRAM went from 10.0 to 13.5 MB when that was fixed.

And the H.264 encoder is reserved at start-up for a reason that is not PSRAM: its
reference frame wants one contiguous block of about 135 KB of *internal* RAM, and
a few seconds later, once the network and TLS have run, the largest free internal
block is 122 KB. Skip that reservation and H.264 can never be built again on that
boot. It looked like the reservation was what starved MJPEG; it was the ring.

With both right, 1080p H.264 holds: 6 fps at 0.2-1.5 Mbit/s against MJPEG's 8 fps
at 9.7, on a screen playing video, at 40 C, with free memory flat over the run.

**What this board does, measured 2026-09-23.** One viewer on the WebSocket, a
screen playing video, the console's second stream fixed (it used to pull the
picture twice), and the numbers taken from `/api/v1/video/status`, which reports
each stage per frame:

| Codec | Mode  | fps       | Byte reordering | Encode   | Bitrate        |
|-------|-------|-----------|-----------------|----------|----------------|
| MJPEG | 1080p | 8.7 - 9.4 | 75 - 79 ms      | 28-30 ms | 17-18 Mbit/s   |
| MJPEG | 720p  | 21 - 23.6 | 24 - 28 ms      | 14-15 ms | 21-23 Mbit/s   |
| H.264 | 1080p | 5.8 - 6.1 | 160 - 164 ms    | 60 ms    | 0.5-1.2 Mbit/s |
| H.264 | 720p  | 16.7      | 54 ms           | 20 ms    | 1-1.8 Mbit/s   |

720p is the mode to give this board: both codecs are about three times livelier
there, because the reordering costs four times less. Between the two at 720p,
H.264 is the one to use over a network - 17 fps at a fifteenth of the bandwidth.
The encoder reads 93-100% busy in every line of that table: the board is at its
limit whatever it is doing, and the reordering is most of it (see the note above
on why that cannot be moved off the CPU).

**The picture comes out flat, and half of that is not ours.** Measured against a
screenshot taken on the source machine itself, 2026-09-19: the capture is the
source through `y = 0.70*y + 31`, and correcting it with `y = 1.42*y - 44` brings
the two to within 1.5 grey levels of each other. That factor is two limited-range
conversions in a row - 0.86*(0.86*x + 16) + 16 = 0.74x + 30. The first is the PC,
which reads the add-on's EDID, decides it is talking to a television and sends
RGB at 16-235; setting the graphics driver's HDMI output range to Full removes
it. The second is the LT6911 converting that to YUV, and nothing here can undo
it: the JPEG engine has no colour-range setting and takes YUV input through
unchanged, and the PPA only applies a range when it converts between YUV and RGB,
which is revision-gated. Boards with a TC358743 do not show this - they capture
RGB888 and the JPEG engine converts it itself, full range.

Comparing against that screenshot is also how the byte order was settled, and it
is worth saying how, because looking at the picture was not enough: every wrong
order still gave a picture that read as "a bit pixelated". What separated them
was taking the error apart by column parity. With the wrong order the even
columns were 1.5 grey levels off and the odd ones 9.9 - the picture was half
right, which no eye reports as half right. With the reversal both parities sit at
1.4.

What is left after that is the flat contrast above, the 4:2:2 chroma the bridge
sends, and JPEG quality: at the default 70 a 1280x720 frame is 176 KB and
coloured text shows chroma blocks, at 80 it is 228 KB and clean.

**The mode is readable after all.** M5Stack publishes nothing about it and
Espressif's driver never asks, but the chip keeps what it has measured in bank
0xe0, found by reading every bank with a known signal on the wire and looking for
the numbers:

| register | meaning |
|---|---|
| 0x80 | pixel clock, MHz |
| 0x88-0x89 | htotal / 2, big-endian |
| 0x8a-0x8b | vtotal |
| 0x8c-0x8d | active pixels / 2 |
| 0x8e-0x8f | active lines |

The horizontal pair counts two pixels at a time, which is how the part moves
them. The refresh rate is not published but follows from the clock and the
totals. All five stay zero for about a second and a half after the chip locks,
and go back to zero when the source leaves - which is what tells the driver there
is no signal, so nothing here has to guess. Changing the mode on the machine at
the other end is enough; the capture follows it within a second or two, with one
torn frame at the moment of the change because the bridge reports the new mode
only after the first frame of it has already gone through a receiver set up for
the old one.

A register dump of both banks, for whoever picks this up, is in
`ignore/m5poe-lt6911-7-dump.log`.

## No audio from the M5Stack Add-on Display In - measured

The LT6911D has I2S and SPDIF outputs of its own, so the add-on could have
brought audio to the P4. It does not, and every line it has is accounted for.
Besides the 24-pin flat cable (two CSI lanes, I2C, power) it sits on two of the
Unit PoE-P4's pin headers, as M5Stack's own product photo labels them:

| Header | Pins | Carries |
|---|---|---|
| SDIO-Bus, 2.54 mm 9-pin | G8-G11, G13 | microSD D0-D3, CMD |
| | G12 | microSD CLK |
| | DP, DN | USB, for the add-on's USB-A socket |
| ISP-Bus, 2.54 mm 6-pin | G37, G38 | UART0 |
| | two pins | marked NC |

Measured on 2026-10-08 with a laptop playing a video into the HDMI input (its
sound went to the HDMI output, and came back to its own speakers each time the
board restarted and dropped the link), with the pulse counter
(`GET /api/v1/system/pinprobe?pins=8,9,10,11,12,13,37,38`): 0 Hz on all of
them, four times over - the card lines held high and CLK low, as an idle card
leaves them, and the UART idle high. The probe was proved on the same board
first: a 48 kHz test signal on GPIO 21 (`&test=21`) read 47 827 Hz. An I2S word
clock would have shown as 44 100 or 48 000. (The Hat2-Bus pins, which the
add-on does not touch, read 0 Hz too.)

M5Stack's schematic settles it
([V0.3, 2026-03-26](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1265/SCH_UnitPoEP4_display_in_V0.3_SCH_PDF_20260326_2026_03_26_16_04_18.pdf),
from the [add-on's page](https://docs.m5stack.com/en/addon/AddOn_Display_In_For_PoE-P4)):
every audio pin of the LT6911D - I2S_D0 to D3, WS, SCLK, MCLK and SPDIF, pins
17-19, 27-29 and 31 - is marked not connected. HDMI audio on this board would
mean wiring to those pins of the QFN itself.

The same sheet has three things worth using:

- **INTIO (GPIO5, pin 26) goes to G38** through a 0 R resistor, with the note
  "When the resolution changes, GPIO5 generates a 200mS high-level pulse". The
  firmware polled the chip over I2C every 200 ms to notice a mode change; it
  now wakes on this pulse as well (`CONFIG_KVM_BRIDGE_INT_GPIO=38`).
- **G37 is TF_DET**, the microSD socket's card-detect switch. Measured, it is
  the opposite of the usual: the switch is open with a card in and pulls the
  line to ground without one, and the 100 k pull-up the sheet draws (R13) is
  not doing the job - left floating, or with an internal pull-down, the line
  read 0 with a card in. With the P4's internal pull-up it reads 1 with a card.
  The firmware now uses it (`CONFIG_KVM_SD_CD_GPIO=37`,
  `CONFIG_KVM_SD_CD_ACTIVE_HIGH`).
- **The chip's CEC pin (33) is wired to the HDMI connector's CEC line.** The
  LT6911D's CEC, if it has a usable one, is not documented; nothing here drives
  it yet.

## The bridge's audio output - read off the wiki, not off a board

Everything above was measured here; this section was not. Nothing in the
firmware captures audio yet, so none of it is proved.

The TC358743 pulls the audio out of the HDMI stream and hands it over as I2S,
and `set_hdmi_audio()` in `components/tc358743` already turns that on. The C790
brings those signals out on a separate 5-pin connector on its left edge, next to
the HDMI socket. They are not on the 15-pin CSI ribbon, which carries the video
lanes, I2C, 3.3 V and ground and nothing else. Geekworm puts a cable for it in
the box.

| C790 | Signal | Raspberry Pi pin, per Geekworm's diagram |
|---|---|---|
| GND | ground | 6 |
| OSCK | MCLK | **not connected** |
| WFS | LRCK / word select | 35 (GPIO19) |
| SD | data | 38 (GPIO20) |
| SCK | BCLK | 12 (GPIO18) |

**MCLK is absent, so the bridge is the master.** It drives BCLK and LRCK, and
the P4 has to receive in slave mode. The sample rate is whatever the source
sends, and it can change while the link runs - the TC358743 reports it over
I2C, so it need not be guessed.

**Wire the ground even though the two boards already share one.** The CSI ribbon
ties them together and three wires would probably work. But the return current
for I2S would then take the long way round through that ribbon, and the ribbon
carries the MIPI pairs, which spend half their time in single-ended LP mode
referenced to the same ground. The wire is in the cable already.

A board with a Raspberry Pi header takes the cable in the positions above
without rewiring: on the ESP32-P4 Function EV, pins 6, 12, 35 and 38 are GND,
GPIO22, GPIO53 and GPIO27. Watch pin 12 there - GPIO22 is also the round LCD's
default chip select, so a device wearing both wants one of the two moved.

The ETH board is not arranged that way. Pick any three free pins: 15, 16 and 17
sit together beside a ground pad, and the whole of that row stays clear of the
display, whose five defaults are all on the other one. Every board's header
table is still `headerVerified = false`, so read the silkscreen before
soldering.

The voltage the C790 drives those pins at has not been measured either. At
1.8 V rather than 3.3 V the P4 would not read a logic high, and the link would
need a level shifter.

## Things that turned out not to be true

Each of these cost real time. They are recorded so they are not rediscovered.

- **The H.264 encoder does not accept RGB.** The component README lists
  BGR888, but the code gates RGB support behind `CHIP_SUPPORT_MIN_REV >= 300`.
  Below revision 3.0 the encoder takes only `O_UYY_E_VYY` (YUV420 with
  alternating `u y y` / `v y y` line prefixes).
- **Capturing YUV422 on a pre-3.0 chip to save PSRAM does not work for H.264**,
  however tempting it looks: a 1080p frame is 4.1 MB instead of 6.2, and the
  rev 3.x boards do exactly that. (MJPEG is another matter - the M5Stack board
  above captures YUV422 and serves it, with a PPA pass to put the bytes in
  order. What follows is about H.264.) Tried on a P4-ETH (rev 1.3) on
  2026-09-17 and the encoder refused the frames outright - `esp_h264_enc_hw_new(): Un-supported
  h264 picture type parameter, pic_type: 59565955` ("UYVY"), the same revision
  gate as the bullet above - so the device fell back to MJPEG, whose picture
  came out green and purple because without the rev 3.0 colour-mode block in
  the CSI bridge the bytes land in another order - which is fixable, see the
  M5Stack section, but was not known then. The PPA cannot take them
  either: all four of its YUV422 input modes sit behind the same revision gate
  (`ppa_ll_srm_is_color_mode_supported`, `#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV)
  >= 300`), so capture YUV422 -> PPA -> YUV420 -> encoder is closed as well. On
  that silicon the capture stays RGB888 with a PPA pass, which is why those
  boards have about 3 MB of PSRAM free while H.264 runs, against 11 MB on a
  rev 3.x board.
- **Only the PPA can convert colour on this part.** The CSI bridge checks the
  chip revision and refuses below 3.0; the ISP accepts RAW8/10/12 only,
  because it is a Bayer pipeline.
- **A PPA transaction that scales while writing YUV420 never completes.** The
  driver's blocking mode waits forever, taking down the calling task. Convert
  at the captured size; do not scale in that pass.
- **`esp_http_server` does not close the socket when a close callback is
  registered.** Inherited code registered one to track the websocket session
  and never called `close()`, so every connection leaked a descriptor and the
  device died after about twenty page loads - pingable, no web interface,
  until power cycled.
- **EDID must not advertise more than ~81 Mpixel/s.** Two MIPI lanes at
  972 Mbit/s carrying RGB888 cannot deliver more, and an over-advertised mode
  produces a black screen rather than degrading. Excludes 1080p60 (148 MHz)
  and 1280x1024@60 (108 MHz). The arithmetic is in `tools/gen_edid.py`.
- **`esp_cam_ctlr` fixes its DMA transfer length when created**, so a
  resolution change needs a new controller, not just new bridge registers.
- **A browser cannot decode H.264 from this device over plain HTTP.**
  WebCodecs is a secure-context API: on `http://<ip>` `VideoDecoder` is simply
  undefined, however new the browser. Nothing to fix in the client - it starts
  working when TLS does.
- **IDF 6 ships mbedTLS 4, where the legacy crypto API is gone.** No public
  `pk.h` key generation, no `ctr_drbg.h`, no `entropy.h`: keys are generated
  through PSA (`psa_generate_key`) and handed to the X.509 writer with
  `mbedtls_pk_copy_from_psa()`. `mbedtls_x509write_crt_pem()` no longer takes
  an RNG callback.
- **Generating a certificate needs ~10 KB of stack.** app_main has about 3.5 KB,
  and the overflow lands as a "Stack protection fault" in task "main" with a
  backtrace that points nowhere useful. Generation runs on its own task.
- **Under TLS the socket is closed by esp-tls, not by the application.** With a
  plain server, registering `close_fn` makes closing the caller's job; with
  `httpd_ssl_start` the session teardown closes the descriptor first, so doing
  it again can close whatever connection has since been given that number.
- **GPIO 35 is the BOOT button and part of the Ethernet interface at once.**
  Found by probing every free pin while the button was held, because guessing
  is not an option for something that erases a password. Two consequences:
  claiming the pin while the network runs kills the network outright - the
  device stays up, logs happily and stops answering even ARP - and the PHY
  drives that line whenever it is out of reset, so after `esp_restart()` the
  button cannot be read at all. The password reset therefore reads it once,
  early in start-up, and only a power-on or EN reset makes that reading
  possible. Holding the button through a reset changes the strapping byte
  (0x30f becomes 0x20f); the Waveshare still boots from flash.
- **The recovery hold goes AFTER the reset, not through it - the boards differ.**
  The Waveshare tolerates the button being held down across the whole sequence,
  which made it look like the gesture to document. It is not. On the ESP32-P4
  Function EV board, BOOT held while RST is pressed is Espressif's documented
  way into firmware-download mode: the ROM stops there, the app never runs, the
  reset window never opens, and the LCD holds its last frame - which reads
  exactly like a hung board. Verified on hardware; a plain RST press gets back
  out, nothing is damaged. The window is polled in software seconds into
  start-up, so holding through the reset never bought anything anyway. Reset,
  release, then hold.
- **The two ESP32-P4 silicon revisions need separate images, in both
  directions.** A build for pre-3.0 declares max chip rev 1.99 in its image
  header; a build for rev 3.x declares min 3.0. Neither starts on the other's
  chip - it is not "works, only slower". Read straight out of the headers:
  `min_rev=1.0 max_rev=1.99` against `min_rev=3.0 max_rev=3.99`. The bootloader
  refuses the image and the board waits in the loader until something it can run
  is flashed over USB; nothing is damaged. This is why every pre-3.0 board target
  has a `-rev3` twin.
  Waveshare confirmed (2026-08) that the ESP32-P4-ETH ships rev 1.3 today, that
  rev 3.x is still ramping up at Espressif with volume expected around October
  2026, and that **a product code does not identify the revision** - batches
  differ, so ask the seller. Expect the same board to arrive as either chip.
- **Internal RAM is the scarce resource, not PSRAM - and "out of memory" will
  point you at the wrong one.** The P4 here has 32 MB of PSRAM and about half a
  megabyte of internal RAM, shared by TLS sessions, USB, lwIP, mDNS, the SD card
  and the hardware encoders' working buffers. The H.264 encoder failing with
  "No memory for reference frame" was diagnosed for most of a day as a PSRAM
  problem; the log line that ended it printed both heaps and read
  `PSRAM 14330 KB free, largest block 14080 KB`. Print internal and PSRAM, free
  and largest block, before theorising about a leak.
- **A PSRAM buffer that is DMA-capable but not cache-aligned makes SPI copy the
  whole thing into internal RAM.** `MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM` is not
  enough: without cache-line alignment the driver falls back to
  `spicommon_dma_setup_priv_buffer()`, a private internal copy the size of the
  transfer. The round LCD's 115 KB framebuffer did this once a second and took
  the video encoder down with it. Use `esp_cache_get_alignment()` +
  `heap_caps_aligned_alloc()`, and keep single transfers small anyway.
- **macOS will not click through two pointer collections in one HID interface.**
  The composite pointer carried an absolute mouse and a relative mouse in a
  single USB interface, each with its own buttons. macOS moved the cursor and
  scrolled but never registered a click - it could not tell which collection a
  button belonged to. Windows and Linux did not care. The fix is one pointer
  per interface: absolute mouse (with the consumer control) on its own
  interface, relative mouse on another. Found by instrumenting the device to
  count button-down edges received - they arrived fine, so the host, not the
  transport, was dropping them.
- **The HID report queue must never coalesce across a button change.** Motion
  reports are folded to the newest position, which is right; folding a
  button-down report together with the button-up that follows it, though,
  turns a click into nothing. Coalesce only while the buttons match.
- **macOS reads the HID physical dimensions as the input surface aspect.** A
  square logical/physical range (0..0x7fff on both axes) gets letterboxed into
  a 16:9 screen, so the cursor tracks only near the centre. Declaring no
  physical range makes macOS map each axis to the screen directly, as Linux
  and Windows already did.
- **A WebSocket handler is never called for the upgrade.** esp_http_server
  answers the handshake and returns - `/* If the request is websocket
  handshake, then do not call the uri->handler */` - so any `req->method ==
  HTTP_GET` branch in a websocket handler is dead code. Ours claimed the
  control session there, which meant the device never knew where to push
  target and LED state and the console reported "no target on USB" for a
  target that was plainly attached. Authentication has the same problem in
  reverse: a 401 written from a handler lands inside an already-open socket
  and the client reports a malformed frame. The fix for both is
  `ws_pre_handshake_cb` (needs `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`),
  which runs while the request still has headers and can still refuse.
- **The console UART is on GPIO 37 and 38.** Reconfiguring them as inputs -
  which a pin probe naturally does - cuts off the log that the probe reports
  through, and the boot appears to hang at "Calling app_main()".
- **The H.264 encoder has no "force keyframe" call**, but it starts a new GOP
  whenever the configured GOP length differs from the one in force. Alternating
  between two adjacent lengths is therefore a keyframe request.

## The Wi-Fi co-processor: its firmware, and the RAM it cost the P4

Measured on the Function EV (P4 rev 3.2, ESP32-C6FH4 rev 0.2), 2026-09-30.

- **The Function EV's C6 runs the same image as the DFRobot FireBeetle 2's.**
  Byte for byte: esp-hosted-mcu "network_adapter", commit 83efce6, slave
  version 0.0.22, IDF v5.4-dev, built 24 Oct 2024. It cannot report a version
  (the host logs 0.0.0) and has only the streaming SDIO mode. Two OTA slots of
  1.5 MB each, ota_0 in use.
- **The C6 can be flashed by hand on the Function EV** through J2, "PROG_C6",
  a 2x3 header at 2.54 mm with no 3.3 V pin: 1 EN (square pad), 2 NC, 3 TX0,
  4 GND, 5 RX0, 6 BOOT (C6 GPIO 9). The C6 has no buttons: BOOT is a jumper
  from pin 6 to pin 4, reset is a touch of pin 1 to ground. The P4 drives the
  same EN from GPIO 54, so hold the P4 in its bootloader while doing this, or
  it resets the C6 in the middle of a write. Writing a whole 4 MB dump broke
  off part-way every time; writing only the used part (up to 0x120000) and
  erasing the rest went through.
- **esp-hosted 3.0.9 can be installed from the P4** over SDIO, through the old
  image's own OTA commands, and the old 2024 bootloader starts the IDF 6.1
  app. It gains nothing yet: under video its aggregated SDIO mode stalled on
  "no slave credits", while the factory image ran clean. The code for it is in
  the firmware, off.
- **Wi-Fi ate the P4's internal RAM.** esp-hosted took a 1536-byte buffer for
  every packet from internal RAM (its "prefer SPIRAM" option only reaches its
  aligned allocator), and lwIP kept every sent TCP segment there until it was
  acked. Wi-Fi acks come later than Ethernet ones, so loading the console alone
  took internal RAM from 85 KB to 1 KB; TLS stopped opening sessions and
  hardware AES could not get its DMA descriptors. Both allocations now go to
  PSRAM (a link-time wrap of `eh_host_port_dma_alloc`, and
  `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` on the Wi-Fi boards); the lowest point is now
  about 50 KB.
- **Wi-Fi power saving was on.** The default modem sleep woke the C6's radio
  every third beacon, about 300 ms. It is off now.

## Reading a crash dump

A panic writes a dump into the `coredump` partition and the console hands it
over (Diagnostics, or `GET /api/v1/system/coredump`). It is the ESP-IDF flash
image - header, ELF, checksum - so `esp-coredump` reads it raw, against the ELF
of the *same build*:

```
esp-coredump info_corefile --core espkvm-0.42.2-p4-eth.dump --core-format raw \
    --chip esp32p4 build/espkvm.elf
```

Two things to check before believing a backtrace. The version in the file name
has to match the ELF, and the summary prints the crashing app's ELF SHA256 - if
it differs, the dump came from another build and the addresses mean nothing. And
the partition is 56 KB: a dump that did not fit was never written, and the panic
log says `Not enough space to save core dump!` instead.

Releases publish symbol maps, not ELFs, so a dump from someone else's device
needs the ELF rebuilt from that tag - or the maps, which still turn the crash PC
into a function name.

## The microSD write ban was tied to the wrong thing

Below chip revision 3.0 the card is read-only and capped at 4 MHz unless the
slot's IO rail is fed by the P4's own LDO_VO4 - writes time out otherwise, which
is measured and true on every board where that rail *is* the LDO's. The M5Stack
add-on's slot is not: it is on the add-on, fed from there, and the firmware
powers nothing. Tying "may write" to the LDO setting therefore gave the wrong
answer here rather than a safe one.

Checked on hardware, 2026-09-20, with a 128 GB card in the add-on: the ladder
takes the full 40 MHz on the first try with no bus errors, and a screenshot
lands on the card - free space drops by exactly one cluster's worth. So the two
are now separate: `CONFIG_KVM_SD_WRITE_NO_LDO` says "this slot writes without
the LDO", set only where that has been checked on a board. The LDO itself stays
unset here and should stay that way without a schematic - driving that rail from
inside while the add-on feeds it from outside is not something to try on
someone else's hardware.

What it opens up on this board: recording, screenshots, timelapse and the
dashcam, and virtual media reads ten times faster than the 4 MHz floor.

## The PPA will rearrange bytes for anything, if you lie to it about the format

Worth knowing away from the board it was found on. The PPA converts colour only
between formats the silicon allows - on pre-3.0 P4 the YUV modes are gated shut -
but its *byte* rearrangement does not care what the bytes mean. Tell it the frame
is RGB565 or ARGB8888, which only says how many bytes a pixel is, and it will
permute them at DMA speed. Measured on real frames, input `36 95 3f 73`:

| what to ask for | result | what it does |
|---|---|---|
| RGB565, `byte_swap` | `95 36 73 3f` | swaps the two bytes of each 16 bits |
| ARGB8888, `rgb_swap` | `73 3f 95 36` | reverses all four bytes |
| ARGB8888, both swaps | `3f 73 36 95` | swaps the two halves of 32 bits |
| RGB565, `block_offset_x = 1` | the stream moves 2 bytes | a one-pixel shift |

Those cover every rearrangement this firmware has needed. It costs about 27 ms a
frame at 1280x720 and 16 bits a pixel, which is simply PSRAM bandwidth - 3.7 MB
moved at around 136 MB/s. Two things to get right: invalidate the output buffer
(`esp_cache_msync`, M2C) after the PPA writes it, or the encoder's own writeback
puts the allocation's zeroed cache lines back over the frame; and keep the output
picture the full width when the block is narrower than the source, or every row
starts early and the picture shears.

The other blocks that can move bytes around, for completeness: the CSI bridge's
`endian_mode` reverses a whole 64-bit word and has no other state, and the JPEG
encoder's `pixel_reverse` swaps the two bytes of a 16-bit unit rather than
reversing pixels, whatever its name suggests. Both measured, not read.

One more worth remembering: `JPEG_DECODE_OUT_FORMAT_YUV420` is `OUYY_EVYY`,
which is exactly the layout the pre-3.0 H.264 encoder demands. So there is a
hardware route from anything the JPEG decoder reads into that encoder - an
expensive one, but it exists.

## Proving a pixel-format fault instead of squinting at it

Every wrong byte order above still produced a picture a person would describe as
"a bit pixelated", and judging by eye cost hours. What settles it in minutes:

1. Take a screenshot **on the source machine** and pull a frame from the device.
   Same size, same content.
2. Fit the levels first (`y = a*x + b`) or the comparison drowns in a contrast
   mismatch. The fit is informative by itself - here it said the PC was sending
   limited-range RGB.
3. Split the error by column parity. With the wrong order the even columns were
   1.5 grey levels off and the odd ones 9.9. Nobody reports a picture as half
   right, but that is what it was.
4. For each parity, find which source column it matches best. That names the
   permutation outright - "the odd columns come from two to the right" - and the
   fix follows from the name.

## Tools worth knowing about

| | |
|---|---|
| `tools/env.sh` | activate ESP-IDF |
| `tools/install-idf.sh` | reproducible toolchain install |
| `tools/gen_edid.py` | regenerate the EDID profiles |
| `tools/check_hid_desc.py` | decode HID descriptors out of the ELF and check report sizes |
| `tools/check_layouts.mjs` | catch duplicate key positions in paste tables |
| `tools/hid_probe.py` | drive the control channel without a browser |
| `tools/video_probe.mjs` | watch the video channel, dump H.264 for offline decoding |
| `tools/abs_range.py` | confirm the host sees 0..32767 absolute axes |
| `web` -> `npm run dev:mock` | full interface against a simulated device, no hardware |
