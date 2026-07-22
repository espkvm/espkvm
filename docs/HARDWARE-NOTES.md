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
| Flashing port | `/dev/ttyACM0`, CH343 bridge. The other USB-C is USB 2.0 OTG HS |
| Network | NetworkManager profile `espkvm-link` shares `enp0s31f6`; device lands on **10.42.0.151** |
| Toolchain | ESP-IDF 6.0.1 in `~/esp/esp-idf`; cmake and ninja live in the IDF python env, not the distro |

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
settles at 5-7 fps.

So the trade at 1080p is 20 fps of MJPEG against 6 fps of H.264 - and, on an
idle desktop, 8.5 Mbit/s against **170 kbit/s**. H.264 is for links too narrow
to carry MJPEG at all, not for smoothness.

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

## Things that turned out not to be true

Each of these cost real time. They are recorded so they are not rediscovered.

- **The H.264 encoder does not accept RGB.** The component README lists
  BGR888, but the code gates RGB support behind `CHIP_SUPPORT_MIN_REV >= 300`.
  Below revision 3.0 the encoder takes only `O_UYY_E_VYY` (YUV420 with
  alternating `u y y` / `v y y` line prefixes).
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
  (0x30f becomes 0x20f); the board still boots from flash.
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
