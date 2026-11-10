# How it works

## What works, in detail

| | |
|---|---|
| Video capture, following the target's resolution changes | works |
| MJPEG streaming | works |
| H.264 streaming | works; needs HTTPS in the browser |
| Keyboard, absolute and relative pointer, media keys | works |
| Alt+Tab, Ctrl+W and the Windows key reaching the target | works in Chrome and Edge; full screen, with control taken, and the keyboard lock button on (the keyboard with a padlock). With it, Esc goes to the target too, and holding Esc leaves full screen and gives control back. Firefox and Safari have no way to give a page those keys |
| Waking a sleeping target from the keyboard | works; over USB, if the machine allows it. Otherwise Wake-on-LAN or the ATX button |
| Pasting text with a keyboard layout | works; US English, Russian, Czech, Ukrainian, Lithuanian |
| Use from a phone or tablet | works; touch trackpad, pinch zoom and on-screen keyboard |
| Multiple viewers, one in control at a time with takeover | works |
| User-defined key macros | works; the commands are in [SCRIPTS.md](SCRIPTS.md) |
| Command palette: any action or setting by name | works; Ctrl+K or Ctrl+Shift+P |
| Bar buttons: a key, a combination, a macro or a runbook on the bar under the picture | works; up to 8, kept on the device |
| Runbooks: a macro that waits for words on the screen, run on the device | works; `wait Press F2`, then `key f2`. Carries on with the browser closed, and each one is a button in Home Assistant. Text screens only. [SCRIPTS.md](SCRIPTS.md) |
| Scheduler: fire an action on a cron timetable | works; Wake-on-LAN in the morning, a runbook overnight, a reset on a schedule. Needs the clock, set over the network |
| Push notifications, with a screenshot | works; to Telegram (with the screen on the MJPEG codec) or a webhook, when a watched phrase appears or the screen goes blank |
| Settings, capability reporting, diagnostics | works |
| Settings to a file, and back | works; no secrets in the file, and a device's own identity is left alone |
| Firmware update over the network, with rollback | works |
| HTTPS with a certificate the device issues itself | works; the CA is downloadable, which also enables H.264 |
| Bring your own TLS certificate | works; Settings, or `PUT /api/v1/tls/cert` |
| Let's Encrypt certificate for the Tailscale name | works; Settings &rarr; VPN, renews on its own, nothing to import |
| Login, two-factor sign-in, and a physical password reset | works; the second factor is a TOTP app, with eight recovery codes |
| Thermal protection | works |
| Virtual media: boot the target from a disk image | works; from a microSD card (FAT32 or exFAT, MBR or GPT), or a small image in the device's own flash |
| Recording the screen to the microSD card, and screenshots | works; the stream the viewers already get, so nothing is encoded twice. A panel lists them, plays them and downloads them |
| A dashcam: the last minutes kept, saved when something happens | works; in memory, or on the card for boards short of PSRAM. Saves an MP4 with chapters on a stop screen, a watched phrase, the power going off, or a button - and sends it to Telegram |
| Timelapse | works; one frame every few seconds, played back at 25 fps |
| Searching a recording for what was on the screen | works; the screen's text is saved beside the video and the panel plays from the moment. Character modes only |
| Reading a text screen as text (BIOS, boot loader, console) | works; select and copy with the mouse, or read the screen *instead* of the video - a couple of kilobytes where a picture will not fit. Character modes only, and the characters are matched against the fonts a BIOS, a UEFI console and a Linux console draw with - a machine drawing with some other font reads as nothing rather than as a guess |
| Noticing a screen that is one flat colour | works; a stop screen or a blanked output has no characters, but it is one colour and it stays |
| Watching the screen for words while nobody is looking | works; off by default. Give it phrases, it alerts in the log and in Home Assistant |
| Guessing the target's OS from how it enumerates USB | works |
| Screen areas: up to eight rectangles watched for a change, for stillness, for a saved picture or for a colour | new. Notification, dashcam clip, Home Assistant sensor, `wait area` in a runbook. [More](#screen-areas) |
| Device profiles: pointer, gamepad and video settings for each target | new. Picked by the device from what HDMI-CEC or USB says about the target, or by hand. Presets for a Steam Deck, a Switch, a Mi TV Stick and an Android TV box on USB. [More](#device-profiles) |
| Wake-on-LAN | works |
| A gamepad for a console (Switch, original Xbox, Steam Deck, Windows) | works on a Switch (Settings &rarr; Input &rarr; Gamepad &rarr; switch_alone); the wired Xbox 360 pad for Windows and a Steam Deck is not tried on hardware yet. The original Xbox pad (`xbox`) comes from [ZacharyLeahan's fork](https://github.com/ZacharyLeahan/espkvm), where it was tried on a real Xbox; the console's controller port needs an adapter to a USB socket. The console gets a gamepad panel or an overlay over the picture, and a controller on your own computer works through it ([clip](https://www.youtube.com/watch?v=UCSkzGHSKWI)) |
| HDMI-CEC: see the source, send it remote keys, standby and wake | works on boards with a TC358743 capture chip; the device acts as a TV. Only media boxes, consoles, a Raspberry Pi and the like speak CEC - an ordinary PC does not. Tried with a Steam Deck in its dock: name, remote keys in the Steam menus and standby work, wake does not ([clip](https://www.youtube.com/watch?v=hXiaOugYkVM)) |
| A battery-backed clock chip | optional; with one fitted the device knows the time after a restart with no network - file names and two-factor codes need it. A DS3231 (DS3231M, DS3232) works and shows its thermometer too; a PCF8563 / BM8563, PCF85063 or PCF8523 is supported from the datasheet but not tried yet. Found by itself on the capture board's I2C bus, or chosen with its own pins in Settings &rarr; System &rarr; Clock. A DS1307 is not used |
| WiFi - station or its own access point | works; on boards with an ESP32-C6, or an ESP32-C5 for 5 GHz. A rescue hotspot and a captive portal |
| Ethernet with WiFi as the backup | works on boards with both; Connection &rarr; Auto. When the cable is pulled the device moves to WiFi, and back when it returns, with no restart. It answers on both addresses, and the certificate names both. Tried on the Function EV: the console stayed open over WiFi and MQTT came back in 15 s |
| ATX power control (power, reset, power LED) | works; wiring in [wiring.md](wiring.md) |
| The target's serial console | new, not tried on hardware yet; Settings &rarr; Power &rarr; Serial console. A UART on two pins you pick, a VT100 terminal in the console that keeps the boot messages, and a REST tail for scripts. 3.3 V consoles wire straight in, RS-232 through a MAX3232 module; [wiring.md](wiring.md#serial-console) |
| The target's log over the network (netconsole / syslog) | new, not tried on hardware yet; Settings &rarr; Network &rarr; Netconsole. The device listens on UDP, keeps 64 KB, shows it live and can notify on phrases like "Kernel panic". No wiring, and it works when the target's disk is gone. The lines are unauthenticated, so they only ever raise a notification, never an action |
| The device's own log, live | new; Diagnostics &rarr; Live log. 64 KB that follows as lines arrive, with levels and search; the download still has the run before a restart |
| Small status display (IP, link, capture, health) | works; optional. An I2C OLED or a round GC9A01, pins assigned from the console, and the picture can be turned upside down for a panel mounted that way |
| A viewing token for dashboards | works; off until you make one. Opens the stream and the figures, and nothing that can touch the target |
| Home Assistant integration over MQTT | works; off by default, auto-discovered. Sensors, buttons, an update entity, a camera holding a still of the screen |
| VPN - WireGuard or native Tailscale | works; off by default, pick one in Settings. Tailscale needs no port forward or gateway |
| HDMI audio: hear the target, and record its sound | new; on boards with a TC358743 (the C790 and the like), three wires and a ground to pins you pick in Settings &rarr; Audio. Heard in the console over WebRTC on the Function EV; sound in recordings is not tried on hardware yet. The M5Stack Add-on Display In has no audio lines ([why](HARDWARE-NOTES.md#no-audio-from-the-m5stack-add-on-display-in---measured)) |

## The parts

![The target's HDMI goes to the TC358743 capture board, which feeds the ESP32-P4 over MIPI CSI-2. The ESP32-P4 reaches your browser over HTTPS on the LAN, and plugs back into the target as a USB keyboard and mouse.](diagram.svg)

## Video

The bridge is polled for signal state and timings, so a target
switching from an 800x600 firmware screen to a 1080p desktop is followed
without intervention. Encoding is done by hardware - the JPEG engine, or the
H.264 encoder with the PPA converting colour on the way in.

Measured at 1080p, on both silicon revisions:

| | MJPEG | H.264 |
|---|---|---|
| Frame rate, Waveshare ESP32-P4-ETH (rev v1.3) | 20 fps | ~7 fps |
| Frame rate, Function EV (rev v3.2) | 23 fps | 25 fps at 1080p50, 27 at 1080p30 |
| Idle screen | 0 kbit/s (unchanged frames are dropped) | 170 kbit/s |
| Screen in motion | 8.5 Mbit/s | ~500 kbit/s |
| Chip temperature at full load | 46 &deg;C (rev v1.3) | 34 &deg;C (rev v3.2) |

Browsers decode H.264 through WebCodecs, and they only offer it on a secure page.
Over plain HTTP no browser can play it, however new - the console says so when it
cannot.

H.264 goes to the browser one of two ways. WebRTC is the first choice: UDP, so
a lost packet costs one frame and the browser asks for that packet again, where
on the WebSocket's TCP every frame behind a lost packet waits for it. That
matters on Wi-Fi and mobile links. The device's side is its own small stack:
ICE-lite with the device's own addresses only (no STUN or TURN), DTLS-SRTP, and
H.264 split into RTP packets small enough for a WireGuard tunnel. Only video goes
this way; keys and the mouse stay on the WebSocket. When WebRTC cannot connect,
the console uses the WebSocket, and both kinds of viewer can watch at once.

**The gap between those two rows is the silicon.** Below revision 3.0 the CSI
receiver cannot produce YUV420, and the H.264 encoder will not take RGB. So every
frame detours through the pixel accelerator to change colour space, and that
detour costs ~104 ms of the ~150 ms a 1080p frame takes.

On revision 3.x there is no detour: capture hands the encoder YUV422 and it takes
it as it is. H.264 went from ~7 to 22-24 fps at 1080p, and 28 at 720p - as fast
as MJPEG, at a fraction of the bandwidth, and cooler. From 0.69.0 the PSRAM runs
at 250 MHz instead of 200, which only rev 3.x allows, and 1080p goes to 26-27 fps.
From 0.70.0 the target sends 1080p50 by default: 25 fps, with much less delay
(see [How long it takes](#how-long-it-takes)).
From 0.69.1 the frame rate limit is 60 by default, and 720p from a 60 Hz source
runs at 59. On the older silicon the
conversion and the encode at least overlap now instead of running one after the
other. The measurements are in
[HARDWARE-NOTES.md](HARDWARE-NOTES.md).

## Sound

The capture chip takes the sound out of the HDMI stream and sends it as I2S:
a bit clock, a word clock and data. The device listens as the slave, at
whatever rate the source plays (32 to 96 kHz), and brings it to 48 kHz.

In the console the sound is off until the speaker button under the picture is
pressed, and the device encodes nothing while nobody listens. Over WebRTC the
sound is Opus (the audio codec WebRTC uses) in the same connection as the
picture, 20 ms a packet. A console that gets its picture over the WebSocket gets
the same Opus packets on a socket of its own and decodes them with WebCodecs, so that needs HTTPS. After a page
reload the browser keeps the sound back until the page is clicked; the button
pulses until then.

Opus at its lowest complexity takes about 7 ms of each 20 ms on one core. It
runs below the video, so the picture keeps its frame rate with the sound on.

A recording gets the sound too: the same encoder serves the console and the
recorder, so the .ts carries the same Opus packets (48 kHz stereo, 96 kbit/s)
next to the picture, and a dashcam clip has them in its MP4. Recordings made
before 0.68 carry AAC; the console's player and the MP4 conversion read both.
The dashcam in memory keeps the encoder running and holds the last two
minutes of sound, about 2 MB of PSRAM, so a clip's past has sound too. Where the source
sends no sound the track holds silence, so picture and sound stay in step. A
timelapse has none. `GET /api/v1/audio/clip.wav?s=5` hands a script the next
five seconds.

## Input

One composite USB HID device with four parts. A boot-protocol
keyboard, which firmware screens understand. An absolute pointer, so a click
lands where it was aimed whatever the target's mouse acceleration is doing. A
relative pointer, for software that captures the cursor. And the consumer keys.
An old BIOS that does not find the keyboard inside that bundle can be given a
keyboard alone, at USB 1.1 speed: Settings &rarr; Input &rarr; Old BIOS keyboard.
Everything is released when the browser goes away, so a dropped connection cannot
leave a key held down on the target.

One thing to know about the absolute pointer: it addresses the target's whole
desktop, not the single output being captured. On a target with a second display
the desktop is wider than the picture, so the pointer travels further than the
mouse and part of the picture aims at the screen you cannot see. Settings ->
Input -> Several screens -> "Find this screen" fixes that: the console sweeps the
pointer across the desktop, sees where it lands in the picture and saves where
this screen sits, so a click lands where you click. Relative mode is the other
way out - it sends movement, not position. The same sweep fixes a target that
draws black borders round a smaller desktop, a 4:3 one in a 16:9 mode say: there
the desktop sits inside the picture, and the clicks would land short of the
pointer without it.

Settings -> Input -> Pointer also has the scroll speed (a wheel notch and a
touchpad swipe alike) and what your own cursor looks like over the picture
while you have control: the arrow, a small dot so the target's cursor is easy
to see, or nothing.

## How long it takes

The Video readout (click the video figures in the status
bar) shows how long a frame spends in the device and in this browser, and
"Measure the delay" times the whole way round - pointer moved on the target until
it shows here. On a Function EV at 1080p: about 160 ms with MJPEG, and about
60 ms with H.264 over WebRTC at 1080p50, the default.

Most of it is waiting for a whole frame, twice: the target draws and sends one
every 33 ms at 30 Hz, and the device used to start encoding only after the last
line had arrived. On rev 3.x chips the H.264 encoder now starts on a frame while
it is still coming in. It reads the frame top to bottom, a 16-line row at a
time, and at 1080p it is slower than the capture, so it starts two rows behind
the incoming lines and never catches up (where it is nearly as fast, at 720p60,
it starts later, by as much as it could gain). The text reader and the
flat-screen check look at the frame after its encode instead of before.

| H.264, Function EV, mouse to picture | before | now |
|---|---|---|
| 1080p30 | 123 ms | 86 ms |
| 1080p50 (the default EDID profile on these boards) | 108 ms | 58 ms |
| 720p60 | 54 ms | 41 ms |

Measured by moving the pointer over the control socket and timing the first
H.264 frame that shows it, so the browser's decoding is not in these.

At 1080p50 the encoder takes every other frame: 36 ms an encode against 20 ms
frames. The frames in between are kept out of memory altogether - the CSI bridge
is closed while the encoder works and opened as it finishes - so the encoder
starts each frame it takes from the top and does not share PSRAM with frames
nobody encodes. 25 frames a second, against 27 at 1080p30. Before starting, the
encoder also checks how far the DMA has written into the buffer, so a mode whose
timing differs from the estimate cannot make it read lines that have not
arrived.

## Target OS

How a machine enumerates a USB device is a fingerprint. Windows
asks for a Microsoft OS descriptor, macOS reads each string twice, Linux does
neither. So the console guesses whether the target is Windows, macOS, Linux or
Android, and shows it next to the USB status, with the raw trace behind a click.

The guess decides which key combinations the console offers - magic SysRq on
Linux, Task Manager on Windows - and whether the Meta key is labelled Win, Cmd or
Super. A `Target OS` setting overrides it when the guess is wrong, or when the
target never showed enough to tell. If it reads your machine wrong, send that
trace and a later build can learn it.

## Screen areas

Draw a rectangle on the picture (Automation, Screen areas) and the device
watches that part of the screen by itself, with no tab open. It cuts the area
into a 16x16 grid, takes the brightness of each cell five times a second, and
counts the cells that moved by more than a little. That ignores noise and still
sees a thin progress bar creep: for one like that, set the threshold to 1%.

An area fires when it:

- **changes**: that share of it is different from the picture it settled on;
- **stops changing**: nothing moved for so many seconds (an install that is
  done, or stuck);
- **looks like the saved picture**, or **no longer does**: press "Save what it
  shows now" while the screen is the one to wait for;
- **turns a colour**: the colour most of it has, or any change of colour, for
  an indicator on a panel that has no other way to say it.

"Has to last" keeps a blink or a passing cursor from counting. Each area can
send a notification, keep a dashcam clip, and shows in Home Assistant as a
binary sensor (and its colour as a sensor, in colour mode). A runbook waits for
one with `wait area Name`, or for it to go off with `gone area Name`. The areas
are kept in parts of the frame, so a new resolution keeps them in place.

## Device profiles

A Steam Deck works best with a relative pointer, and a Switch wants the
gamepad on its own. A profile keeps such settings for one target, and the
device switches to it when that target is attached.

Default is the settings as they always were. A profile holds only the values
it changes, so a change in Default reaches every profile that does not change
that value. A profile can hold the pointer settings, the gamepad, the target
OS, the keyboard layout, the codec, quality, frame rate, EDID and a few console
options. Network, passwords, VPN, recording and pins are the same for all.

The Settings page edits the active profile, named at the top. "New" starts one
from a preset, a copy of the active profile, or nothing. The OS button makes a
profile for the target attached now.

A rule picks the profile: the start of the target's CEC name, its CEC vendor
id, or the OS guessed from USB. CEC wins over USB, because a Switch and a Steam
Deck look like Linux over USB. The device chooses a few seconds after a new
target settles, with no console open. A profile picked by hand stays until
another target is attached. The gamepad and the EDID are read at start-up, so
when a profile changes one of them the device restarts once to apply it.

Without a profile of your own nothing changes, and an older firmware still
finds Default where it was.

## Every feature is optional

Each one reports whether it is compiled in,
whether the hardware supports it, and whether it is switched on. A control the
hardware cannot support is shown disabled, with the device's own explanation on
it - not hidden, and not left to fail silently. `GET /api/capabilities` is that
registry.

## Recording and screenshots

The record button under the picture writes the
screen to the microSD card, into VIDEO/, as H.264 in .ts files. It records the
same stream the viewers get, so it costs no second encode: on the Function EV the
picture stays at 21-22 fps with a video playing on the target, and no frame is
lost. With HDMI audio on, the sound goes in too. A .ts plays in VLC and most players, and a file cut off by a pulled card or
a power cut plays up to the last few seconds. Files over 3.9 GB continue in a
second file, on FAT32 and exFAT alike: the device keeps every file under 4 GB. The camera button saves a JPEG into
SCREENSHOTS/, on either codec. Both are listed in their own panel, where they can
be downloaded and deleted. Recording needs H.264 and a card the device can write,
and not the whole card handed to the target.

A recording is split into files of 10 minutes and stops after an hour; both are
settings. A runbook can start and stop one (`record 300`, `record stop`,
`screenshot`), and so can Home Assistant. With keystrokes in recordings switched
on, each video also gets a .srt of what was pressed and clicked - shortcuts and
named keys, and typed text either as dots or, if you choose, in full. Put the
.srt next to the video and VLC or mpv shows it.

Play in the recordings panel streams a recording from the card, with seeking and
the subtitles shown on the picture; `captures/file` answers HTTP ranges.

## Searching a recording

With the screen's text saved (Settings, Video), the
recordings panel finds a phrase that was on the target's screen and plays from
that moment. It works on screens a character generator drew - a BIOS, an
installer, a console - not on a desktop.

## Timelapse

One frame every few seconds or minutes, played back at 25 fps, so
a night of a long install fits in a minute. Start it from the recordings panel, a
runbook (`timelapse 60`) or `POST /api/v1/record/start?every=60`. It keeps the
stream's keyframes (about one every two seconds), runs until stopped, and becomes
an MP4 when it ends.

## Dashcam

With the dashcam on, the device keeps the last stretch of the screen
in memory (up to 5 MB: minutes of a quiet screen, about 40 seconds of a playing
video) and saves it to VIDEO/ when something happens: the screen goes one flat
colour for half a minute, a screen alert phrase appears, or the power LED goes
off. It keeps recording 30 seconds after. The clip becomes an MP4 with a chapter
for each event, so it plays in a browser, and with Telegram set up it arrives in
the chat as a video. A button under the picture, `POST /api/v1/record/event` and
a Home Assistant button save one by hand.

The past can also be kept on the microSD card instead of memory: the device then
writes the screen to VIDEO/.dashcam in 15-second pieces all the time and deletes
the old ones. That works on boards with little PSRAM, like the P4-ETH, and
reaches back as far as the setting says. The card cannot be handed to the target
while it runs. A black screen does not start a clip; a coloured one, like a stop
screen, does.

## Virtual media

A disk image is presented to the target as a USB drive it can
boot from - a rescue system, an installer, a live image. The console lists what
is there and lets you pick which one the target sees. Images live in two places.

A **microSD card** holds the large ones. FAT32 and exFAT both work, and so do
MBR and GPT, so a card as it comes out of the packet is usually fine: above
32 GB that means exFAT on a GPT card, which older firmware could not read at
all. On exFAT an image can be over 4 GB, like a full DVD installer; the console
sends such a file in 2 GB parts. Recordings stay under 4 GB a file on either.
The console uploads images to the card and deletes them - but check the card
can be written at all before relying on it. A 256 GB SDXC card here mounted,
read and served images perfectly and refused every single write (a CRC error
with the controller reporting a transmit FIFO underrun), while a 32 GB card on
the same board and firmware wrote normally. One card of each, so not a law -
but Flipper Zero tell their users the same thing for their own SD slot: pick a
well-tested card from a known maker rather than the fastest or largest one. So
if you mean to upload to the card, a small branded SDHC card - 16 to 32 GB is
more space than this needs - is the safer buy.

By default a `.iso` goes to the target as an optical drive, any other image as a USB
disk. The drive answers the commands a real one does - the table of contents,
the drive profile, "a disc was inserted" - which Windows, macOS and some
firmware ask before they read. An image above 900 MB shows up as a DVD, a
smaller one as a CD. Checked with a Linux target so far.

How fast the card runs depends on the board, so the device finds it per card.
It starts at the fastest clock the board allows and steps down (40, 20, 10, 4,
2 MHz) when the card fails to mount, fails a test read, or fails a transfer
later. While the card is idle it tries one step up again. The Media panel shows
the speed it settled at, and Settings > Storage > microSD speed caps it by hand.

The card can be swapped while the device runs. The slot is checked every five
seconds, so a card pulled out disappears from the target within about that, and
one pushed in mounts and is offered again without a restart. Pulling a card the
target is reading is safe in the sense that the device notices and drops the
drive; whatever was reading it will report an error, as it would with any USB
drive yanked mid-read. On a board using WiFi, the ESP32-C6 holds the other slot
of the same SD host, so there a card put in after boot is picked up only on the
next restart - a card taken out is still noticed.

Every supported board with a microSD slot powers the slot's pins from one of
the chip's LDOs, LDO 4 (`CONFIG_KVM_SD_IO_LDO_CHAN`) - read off each vendor's
schematic. Until it was switched on, the card only read at 4 MHz, and on a
rev 1.3 chip it did not write at all. The boards below are the ones checked on
hardware.

| Board | Bus | Card read | Card write | Upload from the console (Ethernet) |
|---|---|---|---|---|
| Function EV (rev 3.2) | 40 MHz | ~8.5 MB/s | ~4 MB/s | ~3.5 MB/s |
| P4-ETH (rev 1.3) | 40 MHz | ~9 MB/s | ~4 MB/s | ~1.5 MB/s |

The target reads the virtual drive over USB at ~9 MB/s when the whole card is
handed over (both boards) and ~7.5 MB/s from an image file (Function EV).

The upload is slower than the card because TLS runs on the chip, and the rev 1.3
chip is the slower one. **Pause the video while you upload.** The encoder and the
Ethernet chip share a bus, and at full frame rate the upload drops to a tenth.
So the video goes down to 2 frames a second by itself for the length of an
upload, which still costs about half; the pause button next to the upload gives
the full speed back.

The other boards with a slot start at 40 MHz as well and write on both chip
revisions, but nobody has run them on hardware yet - if the card steps down on
yours, the Media panel says so, and a report helps.

The M5Stack Unit PoE-P4 has no slot of its own: the card is on its Add-on Display
In, on other pins and fed from the add-on rather than from the chip's LDO. That
was why writes were refused there at first - "may write" was tied to that LDO -
and the two are separate settings now. Checked on hardware: 40 MHz, no bus
errors, and it writes.

The **device's own flash** holds one small image, in a 4 MB partition: enough for
iPXE, memtest or a DOS floppy, with no card at all. Flash writes work on every
board, so that one uploads from the browser, or over the cable with
`tools/fetch-rescue.sh` (netboot.xyz by default). It ships empty, and taking the
partition table that carries it is a one-time full flash - the browser flasher
does it - after which the image updates over the network.

The device can also **download an image itself** from a link: netboot.xyz into
the flash slot with one click in the Media panel, or any http/https URL onto the
card. The file does not pass through the browser, so a phone works, and a NAS
link works too. Over HTTPS it asks for ChaCha20 first: the hardware AES needs
internal RAM on every record, and with video running a download died part-way.

## Reading the screen

When the target is in a character mode - a BIOS setup, a
UEFI boot menu, memtest, a Linux console - the device reads the screen back as
text. The console lets you select it with the mouse, or copy the whole screen.

This is not OCR. A text screen is drawn by a character generator, so each cell is
looked up in the font's own shapes. It either matches exactly, or it comes back
as `�`. Never a guess, never a quiet blank, and no "recognised with errors" to
act on by mistake.

Three fonts are known: the 8x16 a legacy BIOS draws with, the slightly different
8x16 a Linux console draws with, and the 8x19 a UEFI console draws with. So a
720x400 setup screen, a 1024x768 boot menu and a Linux console all read. It costs
one pass over the frame, only in character modes, and only once the picture has
stopped moving. A firmware that paints its setup as a picture has no grid, and is
left alone.

The reading can also be left running with nobody connected. Give the device a
phrase to watch for and it keeps looking with the console closed - which is the
state a KVM is bought for, because the machine that falls over does it at three
in the morning. The alert is raised when the phrase appears and cleared when it
goes. Both edges reach the log, and Home Assistant if MQTT is on.

A runbook takes that one step further: a script of keys to send and phrases to
wait for, run on the device itself. `wait Press F2 to enter setup`, `key f2`,
`wait Boot`, `type` a password - a macro with patience, for the BIOS work that
used to need somebody watching the screen. It fails at the line whose phrase
never turned up, and it stops when told to. Like the reading it is built on, a
wait only sees a text screen. The language, the key names and the limits are
in [SCRIPTS.md](SCRIPTS.md).

## Security

The device serves HTTPS with a certificate it issues itself on
first boot, and asks for a password before it will do anything. The password is
stored as a salted PBKDF2 hash, sessions are HttpOnly cookies, and repeated
failures pay a growing delay. Sessions survive a restart: the device keeps only
a SHA-256 of each token in flash, so a copy of the flash holds no cookie that
works.

Two-factor sign-in is one switch away, in Settings -> Security: after the
password, a six-digit code from an authenticator app (Google Authenticator,
Aegis, 1Password and the like), set up by scanning a QR code the device draws.
Eight one-time recovery codes come with it, for a lost phone. The code depends
on the time, and a device with no internet has no clock after a restart, so
then it is checked against the browser's own clock - and each code works only
once, even across a restart. A script that signs in with the password stops
working when this is on: give it a shared session cookie instead (see
[API.md](API.md)).

A forgotten password - or a lost phone with no recovery codes - is cleared with
the board button, two-factor sign-in included. Reset the board, then
press and hold the button for two seconds, while the panel or the log asks you
to. Hold it *after* the reset, not through it: on boards where that button is
also the ROM's download strap, holding it through a reset drops the chip into
firmware-download mode instead.

A second sign-in can watch and do nothing else. Settings -> Security -> View-only
sign-in sets a password for the user `viewer`: it sees the picture, hears the
sound, opens the recordings and reads the screen as text, and cannot type, click,
touch the power, mount media or change a setting. The device enforces that, not
the console: a view-only session reaches a fixed list of read-only endpoints,
and its keyboard, mouse and serial input are dropped. It asks for no two-factor
code. Changing or removing its password signs every viewer out. The status bar
shows how many other consoles are open, and a click lists their addresses, who
is in control and who signed in as the viewer.

Physical presence is the credential here, because whoever can hold that button
can also unplug the machine this device is attached to. The same gesture puts the
network back somewhere reachable - DHCP, the wired link, no operator certificate
- because a WiFi network that has vanished locks a device away just as a
forgotten password does.

## Heat

The chip is watched, and if it ever gets hot the frame rate is halved
and then encoding stops - but the keyboard, the mouse and the web interface keep
running. A KVM that stops accepting keystrokes because it is warm has failed at
the job it was bought for, at exactly the moment someone is using it to fix
something. In practice it does not come up: 1080p at full rate settles
around 46 C in open air on rev v1.3 and 34 C on rev v3.2, against thresholds of
70 and 85 C.

## Updates

Two app slots, with automatic rollback: an image that fails to come
up puts the device back on the one that worked. This also holds later, once the
new image has been accepted: four crashes in a row without one boot staying up,
and the device starts the other slot by itself. That matters here, because this
is often the only way to reach the machine it is attached to. The console can
check for a published build and install it in one click, once **Offer firmware
updates** is on (Settings -> System; off by default). The browser does the
fetching - the device never reaches the internet on its own.

## Interface

A Vue 3 console served from the device as a single gzipped file of about 70 KB,
with no external fonts, scripts or requests: the device has to work on a network
with no way out.

Keys for the console itself work while the target does not have control: F for
full screen, I, M, A and R for the panels, and ? for the list. With control,
every key goes to the target and Esc gives it back. On a phone, a long press on
a button in a bar shows its name. The browser tab is named after the device and
says "No signal" or "Unreachable" in front when that is the case. Its icon
shows the state too: grey when the device does not answer, a ring during an
update, a blinking red dot while recording, a power sign when the target is
off, a dash with no picture.

Ctrl+K or Ctrl+Shift+P opens the command palette: type a few letters of a
panel, a window, a key combination, a macro, a runbook or a setting, and Enter
runs it. It holds only what this board and this sign-in can do, and the keys
are matched by their place, so it opens on any keyboard layout.

The dot next to "Online" in the status bar also says whether frames are
coming. It pulses while they are drawn, stays green and still while the
target's screen does not change (the device then sends next to nothing), and
turns amber when the device sends frames that stop reaching this tab. Red is
a device that does not answer at all. Settings -> UI -> "Show the
keys sent" adds a strip to the bottom bar with the last keys and combinations
that went to the target.
