# API

Everything the console does is available over HTTP and two WebSockets. This
page lists all of it. The console itself is the reference client: what it
sends is in `web/src`.

## Signing in

The device keeps a session in a cookie, `kvm_session`, good for 12 hours. Sign
in once and send the cookie back:

```sh
curl -sk -c jar -H 'Content-Type: application/json' \
  -d '{"user":"admin","password":"..."}' https://espkvm.local/api/v1/auth/login
curl -sk -b jar https://espkvm.local/api/v1/system/info
```

With two-factor sign-in on, a login without `code` (or with a wrong one)
answers 401 with `"needCode": true`; send it again with `"code"`.

A request without a valid session gets 401. While the default password is
still in force, a session reaches only the `/api/v1/auth/` endpoints.

## Three rules for writes

- **POST, PUT and DELETE must say they are meant.** Send either a JSON body
  (`Content-Type: application/json`) or the header `X-ESP-KVM: 1`. An upload,
  which is not JSON, needs the header. Anything else is refused: a form on
  another site can send neither.
- **A browser's `Origin` must match the device.** curl and scripts send none,
  which is fine.
- **The keyboard, pointer and gamepad endpoints, `serial/send` and the
  snapshot need the agent API.** It is off by default: Settings &rarr; Security &rarr; Agent
  REST API. Without it these answer 403. Reading the screen, runbooks,
  virtual media and power do not need it.

## A viewing token

`POST /api/v1/auth/token` makes a token for a dashboard or a camera
integration, and says what it is once; the device keeps only its hash.
`DELETE` the same throws it away. The token opens only the picture: `/stream`,
`/api/v1/video/frame.jpg` and `/api/v1/video/status`. Send it as
`Authorization: Bearer <token>`, or as `?token=` for a client that can only
be given a URL.

## Endpoints

### Device and settings

| | |
|---|---|
| `GET /api/capabilities` | what this device can do, and why it cannot do the rest |
| `GET /api/v1/settings`, `PUT` | settings, validated and applied as a whole |
| `GET /api/v1/settings/schema` | title, range and help text for every setting; `profile: true` on the ones a device profile can hold; `labels` (what the console shows for each choice), `unit`, and `showIf: {key, eq}` or `{key, ne}` for a setting shown only while another one holds a value |
| `GET /api/v1/regions` | screen areas: each one's rectangle (parts of the frame, 0..1), mode (`change`, `still`, `like`, `unlike`, `colour`), threshold (% of the area that must differ), hold (seconds), colour, `notify`, `clip`, and `live`: whether it is on, how far off it is, its main colour, how many times it fired |
| `POST /api/v1/regions`, `/remember` | replace all the areas (`{"regions": [...]}`, up to 8, names unique), or save what area `{"id"}` shows now for `like` / `unlike` |
| `GET /api/v1/profiles` | device profiles: the list, the active one, whether it was picked by hand, the presets, and what is attached now (`seen`: CEC name and vendor, USB OS). `welcome: true` on a device whose first password was just set (the console asks what it will see), `tourSeen`: the firmware version the console's overview of features was last closed on (`""`: never), `suggest`: a preset id for the attached device when it names itself over CEC and no profile fits it |
| `POST /api/v1/profiles`, `/update`, `/delete`, `/select` | make one (`{"name", "from": "current"\|"empty"\|<preset>, "match", "set", "select"}`), change one (`{"id", "name", "match", "set", "unset"}`), delete one, or pick one by hand (`{"id"}`). Default (id 0) cannot be deleted. Each answers with the list, plus `"reboot": true` when the gamepad or the EDID moved and a restart applies it. Settings read and written through `/api/v1/settings` are the active profile's |
| `POST /api/v1/profiles/dismiss` | `{"welcome": true}` ends the first-run question, `{"tour": "<version>"}` closes the overview on that version, `{"preset": "<id>"}` stops `suggest` offering that preset. Answers with the list |
| `POST /api/v1/settings/reset` | put every setting back to its default |
| `GET /api/v1/system/info` | version, uptime, free memory, chip temperature and revision (`chipRev`, 301 = v3.1), thermal state, network, ATX power state |
| `GET /api/v1/system/memory` | each heap (internal, DMA, PSRAM) with its largest free piece and the lowest it has been, and every task's stack: where it is, its size and how close it has come to the end |
| `GET /api/v1/system/log` | the device's own log, as a file |
| `GET /api/v1/system/coredump`, `DELETE` | the crash dump a panic left in flash, as a file, or throw it away |
| `POST /api/v1/system/update` | firmware image, written to the spare slot. `?trial=600` puts it on trial (60 to 3600 s): it goes back to the old image unless kept in time |
| `POST /api/v1/system/install`, `GET` | `{"version"}`: the device fetches that published release and installs it; GET is the progress. `"trial": 600` as above |
| `POST /api/v1/system/update/keep` | keep the update that is on trial. `trialSeconds` in `/system/info` says how long is left, 0 when none is |
| `POST /api/v1/system/update/rollback` | drop the update that is on trial and restart onto the old image |
| `POST /api/v1/system/boot-slot` | boot the other slot on the next restart |
| `POST /api/v1/system/restart` | restart, for settings that need one |
| `GET /api/v1/system/usbprobe` | the target's USB enumeration fingerprint and the OS guessed from it |
| `GET /api/v1/system/pinprobe?pins=19,20` | how fast each pin is switching, and the level it sat at. Only listens; `&test=<pin>` drives a 48 kHz square wave on a free pin to prove it counts |
| `GET /api/v1/pins` | the board's expansion header, and which GPIOs it leaves free |
| `POST`/`GET /api/v1/wifi/scan` | start a scan, then read what it found (boards with an ESP32-C6) |

### Sign-in and certificates

| | |
|---|---|
| `GET /api/v1/auth/session` | whether a login is required, who is signed in, whether two-factor is on and a viewing token exists, `viewOnly` (signed in as the viewer) and `viewer` (a viewer password is set) |
| `POST /api/v1/auth/login`, `/logout`, `/password` | the session, and changing the password. On the setup hotspot the first password also takes `network` (`ethernet`, `wifi` with `ssid` and `wifiPass`, or `ap` with `apPass`) - the choices are in the session's `setupNetwork` - and the device restarts |
| `POST /api/v1/auth/viewer` | the view-only sign-in: `current` (your password) and `next` (the viewer's password, or empty to turn it off). The viewer signs in as user `viewer`, with no two-factor code. Every view-only session ends on a change |
| `POST /api/v1/auth/2fa/begin`, `/enable`, `/disable`, `/recovery` | set up two-factor sign-in (a secret and its QR code), confirm it with a code, turn it off, or make new recovery codes; the last three take `password` and `code` |
| `POST /api/v1/auth/token`, `DELETE` | the viewing token, above |
| `GET /api/v1/tls`, `PUT`/`DELETE /api/v1/tls/cert` | install your own certificate and key, or go back to the device's own |
| `POST /api/v1/tls/acme` | get or renew the Let's Encrypt certificate now; `{"staging":true}` uses the test CA. Progress is in `acme` of `GET /api/v1/tls` |
| `GET /cert.pem` | the device's CA certificate, to import and trust the device (also on port 80) |

### Video and the screen

| | |
|---|---|
| `GET /api/v1/video/status` | resolution, frame rate, bitrate, encoder load, viewers, whether this mode can be read as text, `frames` / `frameAgeMs` (frames captured since boot and how long ago the last one came), `lagMs` (how long a frame waits and how long sending takes), `profile` (the active device profile's id), and `hdd` (the target's HDD LED is lit or blinking; null when it is not wired) |
| `GET /api/v1/viewers` | the open consoles, one per control socket: `ip`, `control` (holds control), `viewOnly` (signed in as the viewer), `you` (the asker's own sign-in) |
| `POST /api/v1/video/reconnect` | offer the HDMI source a fresh start: a hotplug cycle, or a reset of the capture chip where there is no hotplug line. The target sees a monitor replugged |
| `GET /api/v1/video/frame.jpg` | one frame as a JPEG, on either codec |
| `GET /api/v1/screen/text` | the screen as characters when the target is in a text mode; 204 when it is showing a picture |
| `GET /stream` | MJPEG as `multipart/x-mixed-replace`; answers 409 while H.264 is selected |
| `POST /api/v1/video/webrtc` | WebRTC signaling: `{"sdp": offer}` in, `{"id", "sdp": answer}` out, no trickle. The offer must have one video m-line with H.264 packetization-mode 1 and a SHA-256 fingerprint. 409 while MJPEG runs or WebRTC is off, 503 when it is not ready. The session ends with the sign-in |
| `DELETE /api/v1/video/webrtc?id=N` | close WebRTC session N; 204 |
| `POST /api/v1/video/webrtc/sound?id=N&on=1` | the target's sound in WebRTC session N on (`on=0`: off). The offer has to carry an Opus audio m-line too; nothing is sent until this is on |

### Sound

| | |
|---|---|
| `GET /api/v1/audio` | the HDMI audio receiver: on or not, the sample rate, the level, readers, overruns |
| `GET /api/v1/audio/clip.wav?s=N` | the next N seconds (1..10) as a WAV file |

### Recording

| | |
|---|---|
| `POST /api/v1/record/start`, `/stop`, `GET /api/v1/record/status` | record the screen to VIDEO/ on the card; `?seconds=` for a set length, `?every=` for a timelapse. `audio` in the status says whether the recording has sound |
| `POST /api/v1/record/event` | save a dashcam clip now: the past in memory plus `dashcam_post_s` after |
| `POST /api/v1/screenshot` | save a screenshot to SCREENSHOTS/ on the card |
| `GET /api/v1/captures`, `GET /api/v1/captures/file?path=`, `POST /api/v1/captures/delete?path=` | list, download and delete recordings and screenshots |
| `POST /api/v1/captures/upload?path=`, `GET /api/v1/captures/upload`, `POST /api/v1/captures/upload/cancel`, `POST /api/v1/captures/upload/test` | send a recording or a screenshot to the WebDAV folder in the settings (a recording takes its .srt along), see the queue and how each file went, stop it, or check the folder. With `up_dav_ev` set, finished files are queued by the device itself (`"auto":true`). A queued item with `retryIn` > 0 waits for the server, which did not answer |
| `GET /api/v1/captures/search?q=` | find a phrase in the screen text saved with recordings; each answer says which file and how far in |

### Keyboard, pointer and gamepad

These need the agent API, and answer 409 when no target is attached.

| | |
|---|---|
| `POST /api/v1/hid/key` | `{"modifier","keys":[usage,...],"hold_ms"}`: press a chord, then let go. HID usage codes, not names |
| `POST /api/v1/hid/type` | `{"text"}`: type US-ASCII, up to 80 characters a request; send longer text in several |
| `POST /api/v1/hid/move`, `/click` | `{"x","y"}` in 0..32767 across the screen; `/click` takes `"button"` too and clicks there |
| `POST /api/v1/hid/pad` | `{"press":["a"],"hat":"up","ms":100}` taps buttons and holds them up to 2 s; `{"buttons","hat","lx","ly","rx","ry"}` sets the whole state and leaves it held, `{"buttons":0}` lets go. Names are the Switch's; Xbox words (`lb`, `rt`, `start`...) work too |
| `POST /api/v1/hid/reattach` | present the keyboard and mouse to the target again, as if the cable had been pulled and put back. Does not need the agent API |

### Virtual media

| | |
|---|---|
| `GET /api/v1/storage/images` | disk images on the card and in flash, and which one is active |
| `POST /api/v1/storage/upload`, `/rescue`, `/delete` | manage the images; a file of 4 GB and over goes in parts, each with `&offset=` |
| `POST /api/v1/storage/fetch`, `GET` the same, `POST .../fetch/cancel` | the device downloads `{"url","dest":"card"\|"rescue","name"}` itself; GET is the progress |

### Power, HDMI-CEC and the target's logs

| | |
|---|---|
| `POST /api/v1/power/click`, `/hold`, `/reset` | ATX: tap power, hold power for a hard off, tap reset |
| `POST /api/v1/power/wake` | send a Wake-on-LAN magic packet to the target's MAC |
| `GET /api/v1/cec` | HDMI-CEC: the devices on the line (name, vendor, power state) and the last messages |
| `POST /api/v1/cec/key`, `/power`, `/send`, `/scan` | send a remote key (`{"key":"up"}`), `{"action":"standby"\|"wake"}`, a raw message (`{"hex":"04 8f"}`), or look for devices again; `"la"` picks a device other than the active one |
| `GET /api/v1/serial` | the target's serial console: on or off, pins, speed, byte counts |
| `GET /api/v1/serial/log` | the last `?bytes=` (default 4096) from it, as text |
| `POST /api/v1/serial/send` | `{"text":"root","enter":true}`; needs the agent API |
| `GET /api/v1/netlog` | the target's log over the network (netconsole or syslog): listening or not, port, packets, last sender |
| `GET /api/v1/netlog/log` | `?since=N` for what came after position N (the reply's `X-Log-Pos` is where to ask from next); without it, all 64 KB kept, as a file |

### Automation and notifications

| | |
|---|---|
| `POST /api/v1/runbooks/run`, `/stop`, `GET /api/v1/runbooks/status` | run a saved runbook by name, stop it, see which step it is on |
| `POST /api/v1/schedules/run`, `GET /api/v1/schedules/status` | fire a saved schedule now, or see the device clock and what last fired |
| `POST /api/v1/notify/test`, `GET /api/v1/notify/status` | send a test notification (to every channel), or see whether the last one got through; `ready` says which channels are set up, keyed by their routing setting |
| `GET /api/v1/push`, `POST /api/v1/push/subscribe`, `POST /api/v1/push/remove` | Web Push: the device's key and the browsers it notifies (see NOTIFICATIONS.md) |
| `POST /api/v1/notify/chats` | look up the Telegram chats the bot can write to; the list comes in `GET /api/v1/notify/status` |

What a webhook receives is in [NOTIFICATIONS.md](NOTIFICATIONS.md).

## WebSockets

Four sockets, all behind the session cookie. All numbers are little-endian.
The codes are in [`components/kvm_proto/include/kvm_proto.h`](../components/kvm_proto/include/kvm_proto.h),
a header-only component a client written in C can use as it is.

### `/ws`: input and control

Binary; the first byte is the message.

| Client to device | |
|---|---|
| `0x01` mouse, absolute | buttons u8, x u16, y u16 (0..32767), wheel i8, pan i8 |
| `0x02` mouse, relative | buttons u8, dx i16, dy i16, wheel i8, pan i8 |
| `0x03` keyboard | modifier u8, keys u8[6] (HID usages) |
| `0x04` consumer key | usage u16 (0 lets go) |
| `0x05` release all | |
| `0x06` ping | also asks for the status |
| `0x07` take control | become the controlling client; whoever held it loses it |
| `0x08` gamepad | buttons u16, hat u8 (8 = none), lx, ly, rx, ry u8 (128 = centre) |

| Device to client | |
|---|---|
| `0x81` status | flags u8 (bit 0: a target is attached), keyboard LEDs u8 |
| `0x82` pong | |
| `0x83` control | 0 held by another client, 1 you hold it, 2 free, 3 view-only sign-in (control can never be taken) |
| `0x84` update | phase, then percent |

Input from a client that does not hold control is ignored. The console pings
every 2.5 s.

### `/video`: frames

The first message from the client subscribes: `0x01` for the picture, or
`0x02` for the screen as characters (JSON text frames). Any later message asks for a keyframe.

Every picture frame is a 12-byte header and the payload:

| Bytes | |
|---|---|
| 0 | `'K'` |
| 1 | payload: 1 JPEG, 2 H.264 Annex-B |
| 2..3 | width |
| 4..5 | height |
| 6..9 | sequence number, so a client sees what it missed |
| 10..11 | low 16 bits of the device's milliseconds since boot |

### `/audio`: the target's sound

For a client that gets its picture over `/video`; over WebRTC the sound comes
with the video. `0x01` subscribes. The device then sends one Opus packet a
message, 20 ms of 48 kHz stereo:

| Bytes | |
|---|---|
| 0 | `0x03` |
| 1 | 1 on the first packet after a gap, else 0 |
| 2..5 | timestamp, in 48 kHz samples |
| 6.. | the Opus packet |

### `/serial`: the target's serial console

`0x01` subscribes; `0x02` followed by bytes types them. The device answers
with raw bytes from the target, starting with what is still in its 64 KB
buffer.

## Driving it from an AI agent

A skill that teaches an agent the whole API, traps included, is in
[espkvm/skills](https://github.com/espkvm/skills), and an MCP server with the
same calls as tools is in [espkvm/mcp](https://github.com/espkvm/mcp).
