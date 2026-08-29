# Changelog

All notable changes to ESP-KVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning while it is pre-1.0 (a new feature bumps the minor, a fix
bumps the patch).

## [Unreleased]

### Fixed
- **Enable Waveshare ESP32-P4-WIFI6 SDIO internal pull-ups before WiFi starts.**
  The board's 51k external pull-ups can leave the C6 data-ready interrupt line
  too weak; the existing board-specific setting now takes effect before
  ESP-Hosted claims the SDIO controller.

## [0.41.0] - 2026-08-28

### Added
- **Home Assistant gets a firmware update entity.** It shows what is installed
  and what the project has published, with a button that installs it. The device
  reads the manifest itself now, at most once every six hours - until now only
  the console did that, from the browser, which is no use to a dashboard. Only
  where the device is allowed to fetch (Settings -> the same switch that lets it
  install a published release); with that off there is no entity, because one
  that can never answer is worse than none.
- **A camera with a still of the target's screen.** A button takes one on
  demand, and there is a setting to take one by itself when the screen watch
  matches a phrase - so the notification that says `kernel panic` carries the
  screen along with it. Needs the MJPEG codec: while H.264 runs there is no
  still to take. Off by default, because a 1080p frame is a few hundred
  kilobytes over the broker.
- **The mouse jiggler as a switch, with its interval beside it.** The point is
  an automation: quiet during the day, awake overnight. Turning the switch back
  on restores the last interval you set.
- **Diagnostics worth having when something is wrong:** free internal memory and
  the largest unbroken block in it - the gap between those two is what decides
  whether the H.264 encoder can start - along with skipped frames, which
  firmware slot is running, and why the device last booted.

## [0.40.1] - 2026-08-28

### Fixed
- **The ESP32-P4-WIFI6 now gets the chip's own pull-ups on its link to the WiFi
  co-processor.** That board holds all six SDIO lines high through 51k, where
  Espressif ask for 10k, and one of those lines is how the co-processor says it
  has data. Miss that signal and the link sits there associated, addressed and
  silent, while ordinary commands still work - which is what the board's owner
  measured. esp-hosted never switches the internal pull-ups on and offers no
  setting for it, so the firmware now does, before the driver claims the pins:
  about 45k in parallel with the board's 51k, which lands near 24k.

  Whether that is enough is a question for the board rather than the firmware,
  and it is not settled yet - if it is not, the honest answer is a resistor. On
  by default for that board only, and free where a board is already wired right.

## [0.40.0] - 2026-08-28

### Added
- **The Waveshare ESP32-P4-WIFI6, contributed by [@nwomn](https://github.com/nwomn),**
  who has the board. Capture through the C790 and the USB keyboard and mouse are
  confirmed on hardware, and the header and pin reservations are checked against
  the real thing rather than guessed from a drawing. Published for both silicon
  revisions, and offered by the browser flasher.

  **Its WiFi is not dependable yet, and it is the only link this board has.** The
  co-processor associates and hands out an address, then the data path can stall:
  the SDIO interrupt from the C6 stops arriving while commands still work. It
  looks like a board-level problem rather than firmware - the SDIO lines there
  are pulled up through 51K where Espressif ask for 10K - but it is not settled,
  so treat the board as experimental.

- **A drag keeps its path.** Pointer reports were merged into the latest
  position, which is right while no button is down and wrong while one is: a
  drag reached the target as a press and a release with nothing in between, so
  drawing and drag-and-drop lost everything in the middle. Also from @nwomn
  (#28), validated by drawing on a tablet through the KVM.

## [0.39.0] - 2026-08-28

### Added
- **The Waveshare ESP32-P4-WIFI6-DEV-KIT is a build target.** It carries both
  links: 100M Ethernet on a PoE-capable magjack and an ESP32-C6 for WiFi 6. Its
  pins are the ones we already use - Ethernet as on the ESP32-P4-ETH, the C6 on
  GPIO 14-19, the card slot gated on GPIO 45 - so the overlay declares almost
  nothing. One thing to check before wiring: the USB OTG port is switched
  between HOST and DEVICE by a jumper, and the KVM needs DEVICE. Configured
  from the published schematic and not yet run on one, in both silicon
  revisions (`p4-wifi6-devkit`, `p4-wifi6-devkit-rev3`).

  Two more boards in that family were looked at and left out: the ESP32-P4-Pico
  and the ESP32-P4-Core-DEV-KIT have neither Ethernet nor a WiFi co-processor,
  and a KVM nobody can reach over the network is not much of a KVM.

### Fixed
- **The capture reset pin is not driven on this board.** GPIO 23 is the
  TC358743's RESETN on the ESP32-P4-ETH; here it is an ordinary expansion pin.
  GPIO 6 is also no longer offered as free - it goes to the WiFi
  co-processor's IO2.
- **A board with no wired port can be built at all.** The hostname setting and
  the recorder that puts the IPv4 address in the certificate both assumed
  Ethernet, though both also serve WiFi. Groundwork for boards with no wired
  port; nothing changes for the ones that have one.

## [0.38.1] - 2026-08-28

### Fixed
- **H.264 comes back after MJPEG.** Switching to MJPEG and back left the device
  on MJPEG, while the setting still said H.264. The encoder wants one unbroken
  block of internal memory for its reference frame, 135 KB at 1080p, and asks
  for internal only. That memory fragments as the device runs, so the block was
  there at boot and gone an hour later: 323 KB free on the bench, longest run
  132 KB. The encoder is now left alone when the codec changes, and rebuilt only
  when the resolution does.
- **Three string truncations, found by building with -O2.** The certificate
  authority is named after the hostname plus the last of the MAC, and the MAC is
  what keeps two devices apart; a long enough hostname would have pushed it out
  of the buffer. A path to an image on the card was cut instead of refused.
  Neither is reachable with the values the settings allow.

## [0.38.0] - 2026-08-27

### Added
- **A mouse jiggler.** A machine left alone locks its screen or goes to sleep,
  and then what you were watching is behind a password. Set an interval in
  Settings and the pointer is nudged one pixel and put straight back that often
  - nothing moves on screen, and the target counts it as somebody being there.
  It runs on the device rather than in the browser, because the case it exists
  for is a machine nobody is sitting in front of; a console-side one would go
  with the tab. It stands aside whenever you are using the mouse yourself, and
  does nothing when no target is attached. Off by default.

### Fixed
- **The update popup no longer sits lit up under the restart screen.** Starting
  an update raises a full-screen progress panel over a blurred page, and the
  popup the button was in stayed open behind it - with its own progress bar
  visibly filling through the frosting. It closes as the panel goes up.
- **That popup no longer scrolls sideways.** Long version names pushed the rows
  wider than the popup, and asking for a vertical scrollbar quietly gets you a
  horizontal one as well, so a stray bar slid about under the restart screen.
  The rows shrink and the names end in an ellipsis instead.
- **The mark on the sign-in card is centred.** It sat against the left edge with
  the fields, which was never the intention.

## [0.37.1] - 2026-08-26

### Fixed
- **Installing a published release no longer gives up when the link goes
  quiet.** A download that paused for a moment was read as a broken one and
  stopped with "the download broke off" - the read returns a negative number
  both for "nothing yet" and for "the connection is gone", and the two were
  being treated the same. A quiet spell is now waited out, as an upload from
  the browser already was. Nothing was ever at risk: the image is written to
  the spare slot, so a download that fails leaves the running firmware alone.
- **The list of releases is easier on GitHub's rate limit.** It is fetched by
  the browser, so the 60 unauthenticated calls an hour GitHub allows are counted
  against the address the console is opened from - your own, and never shared
  with other people running ESP-KVM. The list is held for ten minutes now, so a
  reload does not spend another, and if the limit is reached the console says so
  and when it clears rather than showing a bare 403.

## [0.37.0] - 2026-08-26

### Added
- **The screen as text, instead of the video.** A text screen is about two
  kilobytes; the video is megabits a second. Tick "text when the screen is text"
  in the video readout and the console shows the characters and stops the
  stream, so a machine stays workable over a phone tether or any link that will
  not carry a picture - the keyboard still reaches the target, which is most of
  what a BIOS asks for. It is a standing preference rather than a place to go
  back to: with it on, the view follows the target through a boot by itself -
  characters at the boot menu, the picture the moment a desktop paints,
  characters again at the next restart - and the encoder only starts when a
  reading fails. Both directions need two readings to agree, so a screen that
  sits near the edge of being readable does not flip back and forth. The
  readings arrive the way the picture does - pushed along the same socket as the
  device makes them, carrying only what changed - so walking a boot menu moves
  the highlight about as fast as the target repaints it.
  `GET /api/v1/screen/text` is unchanged, for scripts, Home Assistant and
  anything whose socket will not open.
- **The selected row comes through in text.** A menu says which line you are on
  by drawing it the other way round, and text alone loses exactly that. The
  scanner already had to try both polarities to read such a row, so it now keeps
  the answer: inverted cells are reported (`highlight` in
  `/api/v1/screen/text`) and drawn inverted in text mode. It is relative to the
  screen - an installer drawn black on white has nothing highlighted - and the
  blanks inside a highlighted label travel with it.
- **A screen that has gone to one flat colour is noticed.** Reading characters
  cannot cover a modern Windows stop screen: it is a graphical page in a
  proportional font, with no grid to cut. What it does have is a shape a few
  hundred samples can see - nearly all of it one colour, and it stays. The
  device reports how long the picture has been flat (`flatMs` in the video
  status, "One flat colour for" in the video readout) and publishes it to Home
  Assistant as a binary sensor with the seconds beside it. It catches a blanked
  output and a frozen desktop too; what it means is the operator's call.

- **Tests that run without a device, in one command.** `tools/test.sh` covers the
  C that reads a screen as characters and the C that decides a screen has gone
  flat, the console's own logic - the demo's machine, the settings file, the
  keyboard tables, the text layer - and the paste tables. The console side is
  new: node runs the TypeScript as it is, so it costs no dependency, and it is
  where a regression like "fast typing repeats a letter" gets caught before
  anybody else finds it. CI runs the same suites.

- **A viewing token, so a dashboard need not be given the keys.** Home
  Assistant's camera integrations can be handed a URL and little else, and this
  device authenticates with a session cookie - so putting a target's screen on a
  dashboard used to mean turning the login off. A token (Settings -> Security,
  off until you make one) opens the MJPEG stream, one frame and the capture's
  figures, and nothing that can press a key or cut the power. Only its hash is
  kept, so it is shown once.

- **Go back to any published release, not just the other slot.** The two slots
  already let the console boot back one step, which helps only while the image
  you want is still sitting in the other one. Settings now lists what the
  project has published and installs the one you pick - a version from weeks
  ago, or one you skipped. The device fetches the image itself, because the
  browser is not allowed to: the host serving them refuses cross-origin reads,
  whichever way the URL is reached. That means the device talking to GitHub, so
  it is **off until you turn it on** in Settings - a KVM often sits where
  nothing is meant to reach the internet, and ordinary updates do not need it.
  A release that lands badly is still reverted by the bootloader, exactly as an
  upload is.

### Changed
- **Security headers on every answer, and a stricter idea of who is asking.**
  A policy that allows this device and nothing else, no framing at all (a
  console inside somebody's invisible frame, with "force off" under the
  pointer), and no content-type guessing. A WebSocket upgrade from another
  origin is refused before the session is looked at, and a request that changes
  something has to carry either JSON or the console's own header - which a
  cross-site form cannot produce.
- **The hotspot no longer comes up open by default.** With no password set the
  device now makes one up the first time the hotspot starts and prints it in the
  log and on the display, rather than running an open network anybody in the
  building can join. An open hotspot is still available, as a deliberate choice
  (Settings -> Network).

### Fixed
- **A few things found by re-reading the new code rather than by running it.**
  The moment a screen went flat was kept as a 64-bit number written by the
  capture task and read by two others - two loads on this core, so a reader
  could catch half an update; it is 32 bits now. Text mode gave the picture back
  even to somebody who had paused it themselves, and left the last frame showing
  under the characters. The MQTT escaper passed control bytes through into what
  is meant to be JSON. A packed-YUV frame with an odd number of pixels would
  have been sampled one byte past its end. None of these had been seen in the
  wild; all of them were waiting.
- **Text mode reads the screen again while it is up.** Found on hardware: text
  mode stops the video, and with nobody watching the device only re-read the
  narrow modes on its own - so on a 1080p console the characters froze the
  moment the mode was entered. Walking a boot menu moved nothing. An operator
  asking for the text now counts as a reason to read, in any mode the reader
  understands, and four times a second while they are there. The console asks
  again right after a keypress instead of waiting for its poll.
- **The MJPEG codec could not be selected once H.264 was running.** The switch
  was only considered when somebody was watching, and the only ways to watch
  MJPEG - the stream, a single frame, anything the viewing token opens - refuse
  to serve while H.264 runs. So the setting saved, nothing changed, and the new
  viewing token had nothing to open. The codec is settled before that check now.
- **The security headers really are on every answer.** They were a call each
  handler had to remember, and the handlers that send their own body - system
  info, the video status, the auth replies - did not. They are set once, where
  routes are registered. "No such page" and "wrong method" come from inside the
  server and never pass a route of ours, so those carry them too now.
- **The MCP adapter can press the power button again.** Its power calls carry no
  body, so they carried no content type either, and the new "did the operator
  mean this" rule turned them away. It sends the console's own header on
  everything now.
- **Where to switch how the screen is shown.** H.264, MJPEG and text were kept
  in three places - two in Settings, one as a button under the picture. They now
  live in the video readout in the status bar, next to the figures they change:
  the codec as two buttons, and text as a tick under them, because which codec
  to send and whether to read the screen as characters are different questions.
  While the characters are up the readout says so rather than naming a codec
  that is deliberately not running.
- **A long screen-watch list no longer truncates the MQTT payload.** The state
  buffer was sized when an alert was one short phrase; the watch has been naming
  every phrase on screen since 0.34.0, and a full list would have been published
  as unparseable JSON. It is sized from the worst case now.
- **The phone's keyboard stops pushing the picture off the screen.** A virtual
  keyboard covers the window rather than shrinking it, so the console was laid
  out as if it were not there and the browser did the only thing left to it: it
  scrolled the page to reach the field, taking the status bar and the top of the
  screen with it. The keyboard is measured now and the console is sized to what
  is left, so the whole picture stays visible above it and nothing scrolls.
- **A capture board that is not plugged in is noticed.** The probe never spoke
  to the bridge - it only registered an address on the I2C bus - so with the
  ribbon off every later read came back with rubbish off the wire, the device
  started a capture on a chip that was not there and reported "no signal". It
  asks the bus for the address now and checks the answer really is a TC358743.
  When nothing answers the console says video is not available and repeats the
  device's own reason: check the ribbon. Input, media and power carry on as
  before - a missing capture board was never a reason to take the rest down.
  Found on a Waveshare board with the ribbon unplugged.
- **Fit stops blowing a small screen up.** Fit meant "fill the stage", so a
  640x480 BIOS on a big monitor was stretched over the whole window and every
  character came out soft. It now shrinks a picture that does not fit and leaves
  one that does at its own size. The old behaviour is still there as Stretch -
  the button walks Fit, Stretch, 1:1 - for anyone who wants the picture as large
  as the window whatever it measures. The pointer still lands where it is
  pointed in all three.
- **The target's keyboard and mouse work again straight after an update.** A
  restart resets our side of the USB link, but the target's power never drops
  and this port does not always give it a clean disconnect - so the target went
  on addressing a device that had forgotten who it was, and nothing reached it
  until the cable was unplugged and back in. The device now drops off the bus
  for a moment on every boot and comes back, which is all a replug ever did.
- **Control no longer gets stuck with a session that has gone.** Only one
  browser drives the target at a time, and releasing that claim could give up if
  it could not take a lock at once - leaving the claim held by a connection that
  no longer existed. Every other console was then quietly ignored, with no
  notice and nothing to press. It waits for the lock now, as the rest of the
  bookkeeping already did.
- **"Take control" is offered in text mode too.** Text mode stops the video, and
  the notice that says another session is driving was hidden along with the
  picture - so an operator reading the characters found the keyboard dead, with
  no explanation and no way to take over.
- **A picture that froze when H.264 was selected.** Switching codec ends the
  older multipart stream cleanly, and a cleanly ended one raises no error in the
  browser: the console kept showing the last frame it had and never moved to the
  channel that carries H.264. It now follows what the device reports it is
  encoding. The same mix-up could also leave the console retrying a stream the
  device answers with "not this codec" every couple of seconds, forever; with no
  transport that can carry the picture it now says so once.

## [0.36.0] - 2026-08-25

### Added
- **A panel can be pinned open beside the picture.** Panels float over the
  screen on purpose - a picture that resizes whenever one opens makes you find
  everything again - but on a wide screen there is room for both, and then
  covering the target is the worse of the two. The pin in the panel's header
  hands it a strip of the stage instead: the picture shrinks to what is left and
  everything drawn on it stays centred on the picture. Off by default,
  remembered per browser (it depends on the window, not the device), and not
  offered on a phone, where the panel is the whole width.
- **Arrow keys on a phone.** A soft keyboard has no arrows, which makes a BIOS
  menu or a boot list impossible to drive from a phone. Touch mode has a pad
  now: the four arrows in the shape they have on a keyboard, with Esc, Tab and
  Enter around them - missing for the same reason and wanted in the same places.
  Hold a key and it repeats.

### Changed
- **The video figures open where they are shown.** Ten numbers about the capture
  had a panel of their own, which slid over the very picture they describe. The
  headline ones have always been in the status bar; clicking them now opens the
  rest just below, and the rail is one button shorter. The bar itself is
  shorter too: "skipped" waits in the readout, and whether the device answers at
  all stands beside the figures rather than inside them - that is about the
  connection, not the picture.
- **Power is a menu on the rail, not a panel.** Three buttons and Wake-on-LAN do
  not need a panel sliding over the screen - and that panel covered the very
  thing you want to watch while a machine restarts. The button sits at the foot
  of the side rail with the target's power state on it, and the menu opens
  beside it.

### Fixed
- **A demo machine that is switched off looks switched off.** With no power there
  is no text screen, and the drawing fell through to the constellation - so a
  target that had just been shut down showed a running desktop, and Reset, which
  does nothing to a machine that is already off, looked broken. It is a black
  screen and "No signal" now, and the power button brings it back.
  Thanks @DaveDavenport.
- **The install stops crying "longer than usual" on a healthy update.** Writing
  and verifying an image takes past forty seconds on a real board, and the
  console expected twelve, so every update looked like it was going wrong; the
  wait for the device to come back was short by a few seconds too. Both now sit
  past what a healthy device takes. Reported by @petrn (#22).
- **The version badge stops competing with the install screen.** It filled as a
  ring and pulsed while the same install was described full-screen - two things
  telling one story, and the moving one made the version underneath hard to
  read. The badge carries the version; the screen carries the install.
  Reported by @petrn (#22).
- **Clicking the picture answers you.** Taking control is a button, deliberately
  - the first click would otherwise land on the target - but clicking the
  picture did nothing at all, which reads as a broken console. It waves the
  button at you now.
- **The demo points at the control it is asking for.** "Open Media" and "turn on
  Select" are obvious to anyone who knows the console and to nobody else, so the
  button in question glows while the demo waits for it, and the screens name
  where it is.
- **The demo types like a keyboard.** Two keys overlapping - which is what fast
  typing is - made it repeat the first one, because it read every report of what
  is held down as a fresh press. Only the device's own demo was affected: real
  hardware passes the report to the target, which decides what is new.
  Thanks @DaveDavenport.

## [0.35.0] - 2026-08-25

### Added
- **Every release carries symbol maps** - one `espkvm-<version>-symbols.zip`
  with a map per board. A panic prints an address and nothing else, and the
  build that could turn it into a function name is thrown away, so reading
  somebody's crash meant rebuilding that exact release first. Under a megabyte
  for all of them, and it turns an afternoon into a minute (#22).

### Changed
- **One list of checksums instead of one per board.** The per-board files were
  an accident of building the boards in parallel; the release page now carries a
  single `sha256sums.txt`, which is also what people actually check against.
- **The demo runs itself until you take over.** It asked for three decisions
  before anything happened - switch virtual media on, choose an image, press
  Reset - and a first-time visitor has no reason to know that. Now a machine with
  nothing in its drive says so, counts fifteen seconds down - long enough to look
  around and pick an image yourself - then loads one and boots it, and types the
  one command the screen asks for. The first thing you do stops the autopilot for
  good, and the screen tells you what the next step is at every point.
- **Handing control back does not take the pointer with it.** Pressing Esc on one
  of the demo's desktops made the drawn cursor vanish. It stays where it was left
  now, the way a real machine keeps its own pointer, while the screen behind it
  carries on moving; it appears only once you have actually driven, so nothing is
  parked in the middle of a screen nobody has touched.
- **The flock wanders off from a hand that has stopped.** The sheep gather round
  the pointer while it moves; after four still seconds they lose interest and go
  back to grazing, and they come back when it moves again.

### Fixed
- **Virtual media shows what is really in the drive.** The panel read the card
  once, when it was opened, so a medium that changed after that - a second
  operator, an upload finishing elsewhere - left it saying "ejected" while the
  target was plainly booting. It re-reads every few seconds while it is open, and
  a choice made here always beats a listing that was already on its way.
- **No scrollbar down the side of the console.** The page is deliberately one
  pixel taller than the window so Android's captive-portal browser stops
  swallowing downward drags. On a desktop there is no such gesture, and that
  pixel drew a scrollbar for nothing. It is given to touch screens only now.

## [0.34.0] - 2026-08-24

### Fixed
- **A refused firmware image no longer leaves the console behind a frozen
  screen.** The install takes over the whole screen, and when the image was
  rejected - the wrong file, or a download that arrived damaged - the message
  appeared underneath it while the overlay stayed up saying what it had been
  doing a moment ago. Only a reload got you out. The screen comes down with the
  failure now.

### Changed
- **The screen watch names every phrase it finds, and has room for more of
  them.** It reported only the first match, so a machine showing both a panic and
  a failed disk told you about one of them; now the alert lists all of them, in
  the order they were typed. The list of phrases also grew from 127 characters to
  255, which was the real limit - three or four phrases and it was full.

## [0.33.1] - 2026-08-24

### Fixed
- **One slow client can no longer take the whole console down.** The web server is
  a single task, and every accepted connection carried a thirty-second receive
  timeout, so a client that opened a connection and never finished its request
  stopped the device answering anybody for that long - measured at 30.9 seconds on
  the bench. The device still pinged and still served a page now and then, which
  is what made it look like a network fault rather than a stuck server. The wait is
  five seconds now; a firmware or image upload is unaffected, because every place
  the server reads a body already waits out a silence rather than treating it as a
  broken connection. Reported by @petrn (#25).

## [0.33.0] - 2026-08-24

### Added
- **The NANO and the Guition boards get their rev 3.x images.** The ESP32-P4 ships
  in two silicon families, an image built for one will not start on the other, and
  the same product code arrives as either - so a NANO bought in August came up as
  v3.1 and esptool refused every release we had. Both boards now build and publish
  a `-rev3` image beside the plain one, each with its own update slug so a device
  is never offered the other family's build. The browser flasher offers the choice,
  and [docs/FLASHING.md](docs/FLASHING.md) says how to ask a board which chip it
  has. Reported by @levrskn (#24).

## [0.32.0] - 2026-08-23

### Added
- **Settings can be saved to a file and loaded back.** Two buttons at the foot of
  Settings. The file is plain JSON with the firmware version and the board in its
  header, which covers both of the real cases: keeping a copy of a working
  configuration before restoring defaults, and bringing a second device up like
  the first. Passwords and keys are not in it - the device never serves them, so
  they cannot be - and loading a file leaves the device's own identity alone:
  hostname and static addresses stay, because two devices answering to one name
  make two certificates a browser trusts neither of. What a file would change is
  shown before anything is written, and what was skipped is said afterwards
  rather than a bare "done".

## [0.31.1] - 2026-08-23

### Fixed
- **The console no longer looks frozen while the device writes an update.** The
  middle step - the device writing and verifying the image on its own - has no
  bytes to report and only a clock to go by, and nothing was ticking it. So the
  animation and the "of about 12s" line sat still for the whole write and only
  came alive at the reboot. The watch runs its own clock now. Reported by
  @petrn (#23).

## [0.31.0] - 2026-08-23

### Added
- **The hotspot shows a join code you can point a phone at.** While the rescue
  hotspot is up, the panel takes a turn showing the network and its password as
  a QR code, then a turn showing them as text. It only appears where the glass
  can hold a code big enough to read: 128x64 shows it at two pixels a module,
  64x48 and 72x40 at one, and the shorter panels keep the text they had.

### Fixed
- **An update takes on the first try again.** The chip could fault part-way
  through its own restart, and the boot that followed read the new firmware wrong
  and got nowhere. A few seconds later the watchdog reset the board properly, the
  old version came back, and from outside it looked like the update had reverted
  itself. The fault is in ESP-IDF's restart path on chips with external RAM - it
  never needed an update to happen, a plain restart could do it too - and it is
  gone in ESP-IDF 6.1, which the releases are built with now. Updating to this
  version is enough - no cable needed - but the restart that installs it is still
  done by the old firmware, so if the device comes back on the old version, run
  the update once more. From this version on it stops happening. Found from a
  serial log by @petrn (#22).
- **An upload no longer gives up when the device is busy.** Over HTTPS a quiet
  stretch on the socket arrives as a raw mbedTLS "nothing yet", not the timeout
  the code watched for, so it was read as a broken connection and the whole
  upload was thrown away. Live video can starve a socket for half a minute, so
  updating with the console open could die at a few percent. A silence is now a
  silence - in the firmware update, the image and rescue uploads, and every body
  the server reads.
- **The console scrolls again inside the phone's hotspot sheet.** Android shows a
  captive portal in a WebView wrapped in its own pull-to-refresh, and that decides
  whether to take a downward drag by reading the document's scroll position - not
  the panel the finger is on. A page sitting at the very top always looked
  refreshable, so a drag up inside Settings reloaded instead of scrolling, and
  nothing the page did could argue: the decision is made outside the WebView. The
  page now rests one pixel down, which is invisible and answers the question the
  other way.
- **The hotspot page says what that sheet is.** It is not a browser and handles
  the console poorly, so the page now points at the address to open properly -
  and warns that the phone must be told to stay on a network with no internet.
- **The touch controls no longer paint over the settings.** They carried a
  stacking order and the panel did not.
- **The diagnostics and target-OS popups stay on screen on a phone.** They opened
  beside their button, which is right when the rail is a column down the side and
  wrong once it is a row across the top - they went up and to the right, off the
  glass both ways. On a narrow screen they are centred instead.

## [0.30.0] - 2026-08-22

### Added
- **The status OLED no longer has to be 128x64.** SSD1306 panels also work as
  128x32, 96x16, 72x40, 64x48 and 64x32, and SH1106 as 128x32, 96x16 and 64x48.
  The display setting lists panels by controller and size together, so pick the
  line that matches yours: the same chip drives every size and reports none of
  it, so this is the one thing that cannot be detected. Devices already set up
  keep the panel they had.

  A shorter panel simply shows fewer lines, and it drops the least useful ones
  first - the address stays, the uptime goes. A line too long for the glass now
  steps along a character at a time rather than being cut off, and the screen
  waits for it before moving on - a clipped address looks like a real one, which
  is worse than a slow one.

- **The status screens got a proper header.** The screen's name and icon now sit
  in a reversed bar, with a dot for each of the three screens so it is clear
  which one is showing and how many there are. On the health screen the readings
  line up in a column beside their bars.

## [0.29.0] - 2026-08-22

### Added
- **You can watch an update happen now.** It takes over the console: the steps,
  the one it is on, and a fan of rays filling as it goes. Writing the image and
  restarting have nothing to measure, so those fill against how long they
  usually take and then turn into a sweep instead of sitting full. Restarting
  from Settings and switching the network show the same.
- **And the console says which version came back.** A restart ends the session,
  so an update that rolled back used to look exactly like one that worked. Now
  it says "came back on 0.28.0, not 0.29.0". Asked for by @petrn (#23).

## [0.28.1] - 2026-08-22

### Fixed
- **An update that starts and then dies no longer leaves a device you cannot
  reach.** The bootloader can undo an update that never comes up, but that
  protection ends the moment the new image confirms itself - which it does as
  soon as the console answers, because from there it can be re-flashed. Anything
  the firmware starts after that point was outside it, so a crash in, say, the
  MQTT or capture path left a confirmed image failing over and over, and
  power-cycling changed nothing: every boot failed the same way.

  The device now counts crashes that never got anywhere. Four in a row without
  once staying up for a minute, and it starts the other slot instead. Presses of
  the reset button and power cuts do not count - only real crashes - so nobody
  gets a downgrade for being impatient. Reported by @petrn (#22).
- **The MQTT state payload no longer sits on the timer task's stack.** It grew to
  576 bytes in 0.28.0 when the screen alert joined it, on a task that runs on
  about 3.5 KB. It lives in one static buffer now.

## [0.28.0] - 2026-08-21

### Added
- **The device can watch the screen when nobody is.** Give it a phrase or two -
  `no boot device`, `kernel panic` - and it reads the screen once a second with
  the console closed, raising an alert the moment one of them appears and
  clearing it when it goes. Both edges go in the log, and Home Assistant gets a
  "Screen alert" sensor with the matched text, published within two seconds
  rather than at the next state interval. Off by default; while it is off, and
  whenever the target is not showing text, it costs nothing - the resolution is
  checked before the frame is touched at all.
- **EDID profiles that cap the target's resolution.** Alongside the full mode
  list there are now `720p` and `1024x768`, which advertise everything up to that
  and prefer it, in both halves of the EDID - the extension block is the usual
  place a source looks for 1080p, so a cap that only trimmed the first block
  would not be a cap at all. A smaller picture encodes faster and costs less bandwidth, which
  on pre-3.0 silicon is the difference between 7 fps and something a person can
  work in. Text modes stay on offer in every profile, so a BIOS still arrives as
  text. This replaces the `custom` choice, which never did anything but fall back
  to the full list - a device set to it comes up on `720p`, so check the setting
  if you had picked it.
- **The Pins tab shows the board's expansion header, not just a list of GPIOs.**
  A GPIO number says nothing about where to put a wire, and half of the chip's
  pins never reach a connector, so the tab now draws the header as it is printed
  on the board - two columns, pin numbers down the middle where the board has
  them - with what holds each pin. The old list of every usable GPIO is still
  there, one click away, because it answers the other question: what is free.

  Pinouts are in for the Waveshare ESP32-P4-ETH, PoE and NANO boards, the
  Espressif Function EV and the Guition ESP32-P4-M3-Dev. They come from the
  vendors' own diagrams, and the tab says so - check yours against the silkscreen
  before wiring anything.

- **The screen can be read back as text when the target is showing text.** A BIOS
  setup, a UEFI boot menu, memtest, a Linux console: press Select and sweep the
  mouse over the picture as if it were a page, or press Copy and take the whole
  screen. Until now the only way to get a serial number or an error code off a
  BIOS screen was to type it out by hand.

  It is not OCR, and that is the point. A text screen is drawn by a character
  generator - a fixed grid, a fixed bitmap per character - so each cell is looked
  up in a table of the shapes the font has. Either a cell matches and the
  character is certain, or it comes back as `�` and you can see exactly which
  characters were not read; there is no "recognised with errors" to catch you out
  later. It costs one pass over the frame, only in character modes, and only once
  the picture has stopped moving - a firmware that paints its setup as a picture
  has no grid and is left alone.

  Three fonts are known: the 8x16 one a legacy BIOS draws with, the 8x16 one a
  Linux console draws with - which is a different font, differing in five
  printable characters including f and v - and the 8x19 one a UEFI console draws
  with. So a 720x400 setup screen reads, and so do a 1024x768 UEFI boot menu and
  a Linux virtual console, with the text area centred the way a firmware console
  centres it.

  Narrow screens are read as they come. A wide one - a UEFI console filling a
  1080p display is 240 characters across - is read only while you are asking for
  it, because looking at every settled 1080p frame on the off chance it is text
  costs four times as much and would nearly always find a desktop. Press Select
  or Copy and it reads that screen too; the first reading takes about a second,
  because a picture has to hold still before it can be read.
- **Three more layouts for pasting text: Czech, Ukrainian and Lithuanian.**
  Pasting types text out as keystrokes, so it has to know where each character
  sits on the target's keyboard. A character the chosen layout cannot type is
  reported rather than guessed at. The tables are now checked on every push -
  against each other, and against the X keyboard database.

## [0.27.1] - 2026-08-20

### Fixed
- **The PoE and rev 3.x images now get their manifests.** They have been built and
  attached to every release since 0.26.0, but the step that publishes the manifests
  still carried the list of four boards it was written with. So those three images
  shipped with an update check pointing at a URL that was not there, and the
  browser flasher had nothing to offer them from. Nothing in the firmware itself
  changed in this release.

## [0.27.0] - 2026-08-19

### Fixed
- **The status display now comes up in the first seconds of a boot.** An I2C OLED
  shares the capture chip's bus, and that bus was only made once capture started -
  so the panel stayed dark for the first fourteen seconds. That covered the whole
  password-reset window, which is exactly when someone is standing at the device
  looking at it.

### Added
- **The hotspot's password on a mono OLED.** A 128x64 panel is too small for a
  scannable join code, so it prints the network's key as text instead - enough to
  type it into a phone. The round LCD keeps showing the QR code.

## [0.26.0] - 2026-08-19

### Added
- **The round LCD's pins now default per board.** Which GPIOs are free is not the
  same on every board, so the console offered five numbers that were right for
  one of them and wrong for the rest. The NANO's come from @DaveDavenport, who
  worked out which pins that board can actually spare. A device that has already
  been set up keeps its own values - a default is only read when nothing is
  stored.
- **Builds for rev 3.x silicon, beside the existing ones.** The two ESP32-P4
  revisions need different images and neither will start on the other's chip -
  the header carries a minimum *and* a maximum. Espressif is still ramping rev
  3.x up, and a product code does not say which chip is in the box, so both are
  published: `p4-eth` and `p4-eth-rev3`, `p4-poe` and `p4-poe-rev3`. Check the
  boot log, which prints `Chip rev:`. On rev 3.x you also get the faster capture
  path and a writable microSD.
- **A build for the Waveshare ESP32-P4-WIFI6-POE-ETH.** The first supported board
  that takes PoE, so a KVM in a rack needs one cable instead of two. Its Ethernet,
  microSD, WiFi co-processor and button turned out to sit on the same GPIOs as the
  boards already supported, so the overlay is mostly inherited. Configured from the
  published schematic and not yet run on one - the same footing the NANO and
  Guition targets started from.

## [0.25.1] - 2026-08-18

### Added
- **Every boot now says how the last one ended** - power on, a reset by hand, a
  panic, a watchdog, or a brownout - and which slot is running and whether it is
  confirmed yet. When an update is rolled back there is otherwise nothing left
  to say why: by the time anyone looks, the old firmware is running again. These
  are four very different faults and the chip already knows which one it was.

## [0.25.0] - 2026-08-18

### Fixed
- **Keyboard and mouse worked again after the log endpoint took their route.**
  The web server has a fixed table of routes; registration past the end fails
  silently, and the input WebSocket is registered last, so adding one endpoint
  cost the whole control channel while every other page still answered. The
  table is bigger now, and a route that does not fit says so in the log instead
  of vanishing.

### Added
- **The device keeps its own log, and the console can hand it to you.** A ring in
  RTC memory that a restart does not clear - so the log of a boot that failed is
  still readable after the device has come back up on something that works, which
  is the one case a serial cable used to be the only answer to. Diagnostics &rarr;
  Download the log. It holds no passwords or keys; it does name your network,
  addresses and MAC, and the console says so where you download it.
- **The update shows itself on the status screen.** Percentage while the image
  arrives, then verifying, then restarting - and what went wrong if it did.
  An update is the longest thing the device does with nothing to show for it,
  and the console has just been asked to give up its video connection for the
  duration, so whoever is standing at the box has somewhere to look. Works on
  the mono OLED as well as the round LCD: by then the panel is attached.

## [0.24.0] - 2026-08-18

### Added
- **Scan to join the rescue hotspot.** When the device is running its own hotspot,
  the round LCD gives its whole face to a QR code: point a phone at it and it
  joins, no squinting at a passphrase. The hotspot exists precisely because the
  device is otherwise unreachable, so the panel is the only thing that can hand
  you the credentials. The small mono OLEDs sit this one out - a code that size
  is not something a phone can focus on.
- **The certificate now names the address DHCP gave the device.** Typing the IP
  used to land on an untrusted page, because only a static address was ever put
  in the certificate. You could click through for the console, but not for the
  video: a browser refuses a `wss://` stream to a mismatched certificate and
  offers no way to accept it. The address is recorded when it arrives and named
  from the next restart, exactly as the IPv6 addresses already were.
- **The board button now tells you it heard you.** Holding it to reset a forgotten
  password used to be a gesture into the void: no light, no message, no way to
  tell whether the button was even wired. The panel now fills a ring around its
  rim as you hold, empties it if you let go early, and says what it cleared when
  it fires.

### Fixed
- **A WiFi network that disappears no longer locks you out for good.** Choosing
  WiFi holds the wired port down, so if the network moved or changed its password
  the device became unreachable by every route at once - and the one setting that
  would fix it lived in the console you could no longer open. The button reset
  always claimed to put you back on Ethernet; it never actually did. Now it does,
  on that very boot.
- **Hold the button AFTER the reset, not through it.** Every instruction we
  shipped said to hold it down through a power-on. On boards where that button is
  also the chip's download strap - the ESP32-P4 Function EV among them - that
  drops the chip into firmware-download mode instead: nothing boots, and the board
  looks dead. The window is polled seconds into start-up, so holding it through
  the reset never helped anyway.
- **The round LCD no longer starves the rest of the device.** Its framebuffer
  lives in PSRAM but was not cache-aligned, so the SPI driver could not send it
  where it lay and copied all 115 KB into internal memory for every frame - of
  which this chip has about half a megabyte in total, shared with TLS, USB, the
  network stack, the SD card and the video encoder. The panel logged a failure a
  second, and the H.264 encoder could not get its reference frame: no picture in
  the browser, from a display setting. The buffer is aligned now and goes out in
  bands, so a single frame can never take the device down with it.
- **A picture instead of a black rectangle when H.264 cannot start.** The
  hardware encoder needs one contiguous multi-megabyte block for its reference
  frame, and after a long uptime PSRAM can be half free and still not have one.
  The encoder is rebuilt whenever the input resolution changes, so that is when
  it bites. It used to retry on every captured frame - thirty times a second,
  forever, logging the same error each time - while the viewer watched nothing.
  Now it says so once, with the actual memory figures - internal and PSRAM,
  free and largest block, because "out of memory" without them sends you
  measuring the wrong heap - and carries on in MJPEG until the next restart.
  Your codec setting is left alone.
- **The video stream steps aside during a firmware update.** The device serves
  the console, the stream and the upload from one small pool of connections, and
  writes flash with the encoder running. Reported as updates that fail and then
  work on the second or third try (#19 petrn).
- The round LCD no longer runs long text off its edges. A 15-character IP address
  wants more room than a 240-pixel circle has, and the overflow was silently
  clipped rather than shrunk.
- The console no longer offers pins that are already spoken for: the six SDIO
  lines to the WiFi co-processor, its reset line, and the pin that carries SD
  power on the Function EV. Picking one of those used to kill WiFi with no
  explanation - and it was the display driver's own default data/command pin,
  which is why a panel wired there stayed dark.

## [0.23.0] - 2026-08-17

### Added
- **IPv6.** The device now answers on IPv6 as well as IPv4, on Ethernet and on
  WiFi. Nothing to fill in - the address comes from the router - and
  `espkvm.local` resolves to it too. The certificate learns the address and names
  it from the next restart, so `https://[address]/` is trusted the same way the
  v4 one is. There is a switch in Settings &rarr; Network if you would rather the
  KVM stayed off IPv6.
- **Tap the network icon** in the footer to see everything about the connection
  in one place: what it is connected by, the name it answers to, its IPv4 address,
  every IPv6 address with what each one is good for, and the MAC you would put in
  a DHCP reservation. Each one is a link and has a Copy button, because nobody
  retypes an IPv6 address.
- **A network with no IPv4 at all now works.** Tailscale and WireGuard used to
  wait for an IPv4 address that was never coming; over WiFi, `espkvm.local` was
  never announced for the same reason; and the little status screen showed a
  blank address. All three now take the IPv6 route. Wake-on-LAN genuinely cannot
  work there - a magic packet is an IPv4 broadcast, and IPv6 has none - so it
  says so and hides the button instead of failing quietly.
- The device now appears in network discovery under **its own hostname** rather
  than as one of several identical "ESP-KVM" entries. Only the name you chose is
  published: no firmware version, no board model. mDNS is shouted at every device
  on the link, and a version number there is a free list of which bugs apply.

## [0.22.1] - 2026-08-13

### Fixed
- **Video no longer falls apart while you watch.** The frame pump polled with a
  5 ms delay that, at this firmware's 100 Hz tick, rounds down to zero - a busy
  loop that starved the chip whenever a viewer was connected: stutter, artefacts,
  and eventually a stream only a reboot would bring back. The pump now sleeps on
  the frame signal itself, which also shaves a little latency.
- **A heavy screen change repairs itself at once.** Opening a window is a burst of
  large frames; when the sender falls behind and skips one, the H.264 picture
  shatters until the next keyframe. The device now notices the skip and asks for a
  keyframe immediately - a blink instead of a two-second smear.
- A viewer that stops reading - a closed laptop lid, a dead link - is skipped and
  then dropped, instead of slowing the stream for everyone else.
- The update popup showed its percentage in three places at once; now it's one bar
  and one line. It also says which slot the update will install to (the inactive
  one - the running slot is untouched until the new image verifies).

## [0.22.0] - 2026-08-12

### Added
- **See both firmware slots - and boot the other one.** The firmware panel now
  lists both app slots with the version on each, which one is active, and whether
  it's confirmed or still unconfirmed. A "Boot this" button switches onto the other
  slot and restarts - handy for dropping back to a known-good image without
  reflashing. A slot with no valid image is refused, so a click can't strand the
  device.

### Fixed
- **The Function EV board boots cleanly again** (and the NANO/Guition targets).
  esp-hosted 3.0 moved its boot-time init, so the guard that keeps the WiFi
  co-processor off the shared SD bus in Ethernet mode quietly stopped working: the
  onboard C6 grabbed the bus, the microSD fought it for the card, and the board took
  ~30s to start while logging an endless SDIO error. It's back to a few seconds.
  Boards with no co-processor (the Waveshare P4-ETH) were never affected.
- **A glitchy H.264 picture repairs itself.** The browser's video decoder can
  silently stall - the image fills with artefacts and even a keyframe won't clear
  it. The console now spots that no decoded frames are coming out, rebuilds the
  decoder and resyncs on the next keyframe instead of needing a reload; if it still
  can't recover, it falls back to the MJPEG stream.
- A just-installed over-the-air image is now confirmed before the optional network
  features (MQTT, WireGuard, Tailscale) start, so a hiccup bringing one of those up
  can't leave a perfectly reachable update unconfirmed and bound for rollback.

## [0.21.2] - 2026-08-12

### Fixed
- **Pressing `f` no longer flips the browser to fullscreen while you're typing.**
  The console's `f`-for-fullscreen shortcut fired even inside a text field, so
  typing an `f` into the login password - right after an OTA update or a reboot,
  before you're logged in - toggled fullscreen instead of entering the character.
  Most visible in Safari, where it looked like an unasked-for Cmd-F. The shortcut
  now stays out of the way while a field is focused or a modifier is held. Thanks
  to the reporter in [#16](https://github.com/orgs/espkvm/discussions/16).

## [0.21.1] - 2026-08-11

### Fixed
- **An update now tells you where it is.** The bar used to fill up and then sit
  there while the device quietly wrote and verified the image, so a working update
  looked like a hung one ([#13](https://github.com/orgs/espkvm/discussions/13)). It
  now names each step - downloading, uploading, writing, restarting - waits for the
  device to answer again, and reloads the console onto the new firmware by itself.
  If it doesn't come back, it says so instead of leaving you guessing.
- An update that fails now leaves the reason on screen in the firmware panel, rather
  than in a toast that fades while you're watching the progress ring. A file that
  clearly isn't a firmware image is turned away before the upload starts.
- A dropped connection on the last byte of an upload is no longer reported as a
  failure - that's what the device restarting looks like from the browser, so the
  console waits it out and checks.

## [0.21.0] - 2026-08-11

### Added
- **A little status display.** Solder on a small OLED (SSD1306 or SH1106,
  auto-detected on the capture I2C bus) or a round GC9A01 colour LCD, switch it on in
  Settings, and the device shows its IP, link, capture status and health right on the
  panel - a boot logo first, then cycling pages with icons and temperature/RAM bars.
  Off by default, and it stays out of the encoder's way so streaming is unaffected.
- **Assign GPIO pins from the console.** Pin settings are now drop-downs of the free
  GPIOs (plus "None"), and a new Pins tab shows the whole map at a glance - what the
  board reserves, what you've assigned, and what's free. The ATX pins live on that map
  now too, so they can't quietly collide with anything else.

### Fixed
- Scrolling the settings page no longer changes a setting when the pointer happens to
  land on a drop-down - which could silently flip the display type and blank the screen.
- The round LCD now shows the Tailscale address as well, matching the OLED.

## [0.20.0] - 2026-08-10

### Added
- **Virtual media now serves the right kind of drive on its own.** An `.iso` is
  presented to the target as a CD-ROM, so installers boot and mount as an optical
  drive; anything else comes up as a removable disk. Nothing to set - though you can
  still force CD-ROM or disk if a file is misnamed. Switching between the two re-plugs
  the USB drive for you.
- **Hand the whole microSD card to the target.** A new "Whole microSD card" item in
  the Media panel exposes the entire card as a USB drive - every file, not one image.
  On rev-3.x boards it's read-write, so the target can copy files onto it; while the
  card is handed over the console steps off it so there's a single owner, and re-reads
  it when you switch the medium back (a reformat by the target is fine).
- **Uploads show throughput and a time estimate**, so a slow multi-gigabyte write to
  the card visibly moves instead of looking hung.

### Changed
- **The Media panel is now the one place to pick and turn on virtual media** - it has
  its own on/off switch (with the restart reminder), the rescue slot shows its size,
  and the duplicate "Mounted image" field is gone from Settings.

## [0.19.2] - 2026-08-09

### Fixed
- ATX power control could never be turned on: the settings that configure it (the
  button GPIOs and the enable toggle) were hidden until ATX was already working -
  which needed those very settings. A deadlock. The Power settings are now always
  visible, so you can wire up the optocouplers, set the GPIOs, and enable it. Thanks
  to marcopompili for the report.
- OTA updates sometimes finished but didn't reboot on their own (issue #13, thanks
  petrn). The restart was fired inline from the request handler right after the
  upload, where a blocked response send or an already-closed connection could swallow
  it; it now runs on a short timer and reliably reboots once the new image is written.

## [0.19.1] - 2026-08-07

### Changed
- microSD is now **writable on rev-3.x boards** - you can upload and delete images
  from the console. The write path was there all along but gated off, because the
  older ESP32-P4 stepping times out on SD writes; rev 3.x handles it fine (verified
  on hardware). Pre-3.0 boards stay read-only.

### Fixed
- A Linux machine that re-reads a USB string descriptor (an ASUS NUC on Ubuntu
  re-reading its serial number) was being detected as a Mac. The OS guess now also
  checks that the langid is requested last - the macOS tell - so Linux reads as Linux.

## [0.19.0] - 2026-08-07

### Added
- **Headscale (self-hosted Tailscale).** Point the device's native Tailscale client
  at your own Headscale or Ionscale control server instead of Tailscale's cloud -
  just a control-server URL (and optional port) in the VPN settings.

### Changed
- The target now sees the "monitor" as **ESP-KVM** instead of the capture chip's
  Toshiba name.

### Fixed
- The Waveshare ESP32-P4-NANO and Guition ESP32-P4-M3-Dev now build for pre-3.0
  silicon by default. The units a contributor actually tested turned out to be
  pre-3.0, and the 0.18.0 images (built for rev 3.x) wouldn't boot on them. Capture,
  USB and Ethernet are confirmed working on both; rev-3.x owners can flip a config
  for the faster H.264 path.
- The PWA install splash was blurry on Android - the icons were marked maskable
  without any padding, so it zoomed into the logo. They're plain "any" now, so it
  renders crisp.

## [0.18.0] - 2026-08-06

### Added
- Draft build targets for two more boards: the Waveshare ESP32-P4-NANO and the
  Guition ESP32-P4-M3-Dev. Both are configured from their datasheets and haven't
  been run on hardware yet - in theory they should work, but a few pins still need
  confirming. They show up in the browser flasher labelled "configured, untested".

### Changed
- Clearer firmware download names: the Waveshare ESP32-P4-ETH build is now
  `p4-eth` (was `waveshare`), since more boards are on the way. Devices already in
  the field keep updating as before.

## [0.17.0] - 2026-08-06

### Added
- **WiFi**, on boards with an ESP32-C6 (like the ESP32-P4 Function EV). The P4 has
  no radio of its own, so it talks WiFi through the C6; the Waveshare board is
  unaffected. Pick your link in the new "Connection" setting - Ethernet, WiFi, or the
  device's own access point - and swap it any time from the network pill in the status
  bar. One P4 caveat: the microSD and the C6 share a bus, so virtual media only works
  in Ethernet mode.
- **Rescue hotspot for WiFi.** If the device can't reach its network, it can also put
  up its own hotspot (`ESP-KVM-xxxx`) so you can still get to it and fix things -
  meanwhile it keeps trying the network and reconnects on its own once it's back.
  Turn it on with the new "If WiFi can't connect" setting.
- **Captive portal on the hotspot.** Join the device's access point and your phone
  opens the console on its own, like hotel WiFi. Over the hotspot the console runs on
  plain HTTP (a captive browser won't accept the self-signed cert), so H.264 is off
  there but MJPEG and settings work; Ethernet and WiFi keep full HTTPS.

### Changed
- **1080p H.264 is faster on rev 3.0+ silicon** - roughly 15 -> 22 fps. A third
  capture buffer keeps the encoder fed instead of waiting on the camera, and dropping
  some buffers the direct path never used freed the memory for it.
- **rev 3.0+ now captures YUV422 straight into both encoders**, skipping the
  colour-convert pass. Frees ~4 MB of PSRAM and puts H.264 and JPEG on one format; the
  older rev <3.0 path is untouched.

### Fixed
- **Two devices on the default hostname could clash on certificates** - their
  self-issued CAs had the same name, so trusting one made the browser reject the other
  (`ERR_CERT_AUTHORITY_INVALID`). Each CA now carries a per-device suffix and is
  re-issued automatically, nothing lost.
- **The rescue hotspot was hard to join while WiFi was hunting for its network** - the
  constant channel scanning kept knocking the hotspot off its channel. It now paces the
  retries so the hotspot stays put.
- **A fresh clone didn't build** - `sdkconfig.defaults` didn't set the chip, so the
  build fell back to `esp32` and failed on a missing toolchain. It now targets
  `esp32p4` out of the box.

## [0.16.5] - 2026-08-05

### Fixed
- At high resolution the free-running CSI capture DMA could overwrite the frame a
  codec was still reading and tear it: an encode takes longer than one capture
  period, so the two-buffer ping-pong came back around mid-read. The capture now
  holds the frame being encoded out of the DMA rotation (and drops intermediate
  frames to the driver's backup buffer) so the buffer under the encoder is never
  written.

## [0.16.4] - 2026-08-05

### Added
- Native Tailscale now chooses its home DERP relay region by latency instead of
  always relaying through the built-in default (Dallas). On first connect the
  device probes every region in the tailnet's DERP map and relays through the
  nearest, falling back to Frankfurt and then the default if none answer - a large
  latency win for tailnets reached from outside North America (microlink #19).
- The WireGuard settings show the device's own public key with a Copy button when
  WireGuard is selected, so it can be registered as a peer on the hub without
  digging it out of the raw `/api/v1/system/info`.

### Changed
- The pointer-mode setting dropped its "auto" choice, which did nothing (it
  behaved as "absolute"); it is now just Absolute / Relative.
- A tagged release's GitHub notes are taken from the matching CHANGELOG section
  rather than auto-generated commit summaries.

### Fixed
- WireGuard did not come up after a cold boot when its endpoint was a DNS name:
  the address was resolved before the network was ready and never retried. It now
  waits for the link (as Tailscale already did) and retries, so the tunnel
  establishes on its own.
- A key or mouse button could stay stuck down on the target when a USB report was
  dropped while the endpoint was momentarily busy (e.g. the host stalled the
  transfer): reports are retried, the release-all safety path can no longer be the
  one dropped, and a stale completion no longer makes the next report skip its wait.
- In an installed PWA the bottom action bar (and the top status bar) could sit
  under the phone's home indicator or notch and look missing - most visibly in
  dark theme. The bars now respect the safe-area insets.
- Repeated H.264 keyframe requests (one per reconnecting viewer) walked the GOP
  length down toward 2 over time, turning almost every frame into a keyframe and
  wrecking the bitrate. The keyframe toggle now stays around the configured GOP.
- Switching the video codec while a slow client still held a frame could leave
  later frames tagged with no payload type, so clients mis-decoded them until the
  next switch.
- The "Power LED active-high" setting never persisted: its NVS key exceeded the
  15-character limit, so it reverted to the default every reboot and could leave
  the reported power state inverted.
- The thermal guard could jump straight to stopping video if the warn and stop
  thresholds were set out of order; the warn threshold is now kept below stop.
- A virtual-media image of 2 GB or larger uploaded as an empty "success" (the
  length was truncated to a signed 32-bit int); large uploads now transfer fully.
- Hardened the control WebSocket against a zero-length frame and tightened a few
  internal bounds and authentication checks.

## [0.16.3] - 2026-08-04

### Fixed
- The USB status indicator (and the REST "no USB target attached" checks) could
  report no target after a warm reboot with the cable still attached, even though
  the keyboard and mouse worked: the readiness flag came from the USB mount event,
  which is missed if the target enumerated the device before the HID task
  registered its handler. Readiness now reads TinyUSB's own mount state directly,
  so the indicator matches reality without needing a replug.

## [0.16.2] - 2026-08-04

### Changed
- The VPN settings tab is a single Off / WireGuard / Tailscale selector that shows
  only the chosen backend's fields (and nothing when off), instead of listing every
  WireGuard and Tailscale field at once and making the operator scroll past the one
  they are not using. The tab is now just "VPN".

## [0.16.1] - 2026-08-04

### Fixed
- Enabling a VPN (Tailscale or WireGuard) before it was fully configured locked
  its own settings: an incomplete tunnel reported its capability as unavailable,
  which disabled the very fields - including the Tailscale auth-key box and the
  enable toggle - needed to finish setting it up, with no way back without a
  reflash. The VPN capabilities now track only whether the subsystem is present
  (always, here), so their settings stay editable; connection state is reported
  through the status API and the VPN pill instead. Secret settings (VPN keys) are
  also marked as such in the settings schema, so the console renders them as
  write-only password fields that keep the stored value when left blank.

## [0.16.0] - 2026-08-04

### Added
- Native Tailscale, as a second VPN backend alongside classic WireGuard (enable
  one in Settings -> VPN). The device joins a tailnet directly - no gateway, VPS
  or port-forward - and is reachable at its 100.x address (or MagicDNS name) from
  anywhere, with NAT traversal (DERP/DISCO) handled for it. Built on a native
  ts2021 client (microlink) ported to ESP-IDF 6. Set a Tailscale auth key and,
  optionally, a tailnet hostname; `/api/v1/system/info` reports the tailnet state
  (enabled/up/address/peers). The console's TLS certificate is re-issued to name
  the tailnet address and MagicDNS name, so it is valid when reached over
  Tailscale. Runs on its own worker task; bring-up never blocks boot.
- Installable PWA console. The web console ships a manifest, service worker and
  icons, so a phone can install it to the home screen and run it full-screen and
  standalone - which also removes the mobile browser chrome that shrank the
  usable area. Touch control is a proper relative trackpad with acceleration.
- The generated device CA is now named after the device (e.g. "espkvm ESP-KVM
  CA") so it is identifiable in a phone's trusted-credentials list, and `/cert.pem`
  is served as a CA certificate (`application/x-x509-ca-cert`, `.crt`). The
  console is served `no-cache` with a version ETag so a firmware update always
  delivers the matching console, and an open tab is offered a reload when the
  device is updated under it.

### Changed
- The classic WireGuard client now runs on the same bundled WireGuard stack as
  Tailscale, instead of a second, separate one. This removes a symbol clash so
  both can live in one firmware and be chosen at runtime; the WireGuard feature
  and its settings are unchanged for the operator.

### Fixed
- The self-signed certificate's serial number could be an invalid ASN.1 integer
  (a redundant leading zero) roughly one time in 256, producing a certificate
  some clients rejected outright. The serial is now always a valid positive
  integer.

## [0.15.0] - 2026-08-03

### Added
- Bring-your-own TLS certificate. An operator can install their own certificate
  and key (e.g. from an internal CA or a real public one) so the browser trusts
  the device without importing the device CA. `PUT /api/v1/tls/cert` takes one
  PEM blob (the certificate chain, leaf first, then the private key - exactly
  `cat fullchain.pem privkey.pem`); `DELETE` reverts to the self-signed identity;
  `GET /api/v1/tls` reports which is in use. The pair is validated (both parse
  and the key matches the certificate) before it is stored, and a bad upload can
  never strand the console: if the TLS stack rejects the certificate at start-up,
  the server falls back to the self-signed one.

## [0.14.0] - 2026-07-31

### Added
- WireGuard VPN client (Settings -> VPN), off by default. The device reaches a
  peer over the existing Ethernet link as a split tunnel - only its own tunnel
  address rides WireGuard, so the console stays reachable on the LAN too. It
  generates its own X25519 key on first use (via PSA) and reports the public key
  to add to the peer; the operator supplies the peer key, endpoint and tunnel
  address. Optional SNTP keeps the handshake timestamp valid across reboots.
  `/api/v1/system/info` reports the tunnel state (enabled/up/address/publicKey).
  Bring-up runs on its own worker task, so a slow or failing connect never blocks
  boot or the web server.
- Agent / computer-use REST endpoints, so an AI agent (or any script) can drive
  the target without the binary WebSocket protocol: `GET /api/v1/video/frame.jpg`
  for a single JPEG still, and `POST /api/v1/hid/move`, `/hid/click`, `/hid/key`
  and `/hid/type` for pointer and keyboard input. Coordinates are the raw HID
  range 0..32767; the caller maps screen pixels using the resolution from
  `/api/v1/video/status`.
- An "Agent REST API" setting (Security), off by default, that gates all of the
  above. These endpoints grant the same keyboard/mouse/screen control the console
  already has, over a simpler interface, so they exist only when the operator
  turns them on; each still needs a session and a USB target.

### Fixed
- The HID endpoints never block the web server. Each only enqueues to the HID
  worker and returns; the worker paces the reports over USB. Blocking the single
  server task (an earlier draft used a per-character/hold delay) stalls every
  other request and backs up the TLS handshake pool, which could hang the whole
  web server under load - the same reason the upload and MJPEG-stream paths run
  on their own tasks.
- Settings that do not depend on hardware - the whole MQTT/Home Assistant
  section and the agent-API toggle - are no longer hidden on a device without
  video capture. They had defaulted to requiring the video capability; they now
  declare themselves always applicable.

## [0.13.1] - 2026-07-31

### Added
- A connection indicator for the MQTT bridge in the status bar, next to the
  HDMI/USB/SD/Ethernet icons. It appears only when MQTT is enabled, and hovering
  it shows whether the device is connected to the broker or still connecting.
  The firmware reports this in `/api/v1/system/info` as `mqtt.enabled` and
  `mqtt.connected`.

### Fixed
- MQTT: turning the bridge off (or changing its settings) now publishes
  "offline" before disconnecting, so Home Assistant no longer shows the device
  stuck "online". A clean disconnect does not trigger the last will, so the
  retained availability had to be updated explicitly; an unexpected drop still
  falls back to the last will.

## [0.13.0] - 2026-07-31

### Added
- Home Assistant integration over MQTT. Turn it on and the device is
  auto-discovered as one Home Assistant device: sensors for temperature,
  viewers, video mode/frame rate/codec/bitrate, uptime, free PSRAM, HDMI
  signal, target USB and target power, plus buttons for power, reset,
  force-off, Wake-on-LAN and restart. Availability is tracked with a last-will
  topic. TLS is supported (verify against the built-in CA bundle, or skip it
  for a self-signed broker). Off by default and gated by the `mqtt_*` settings,
  so a device that never enables it only pays the linked code. esp-mqtt is a
  managed dependency now that it has left the IDF core in v6.

### Security
- The must-change-password state is enforced on the device, not only in the
  console: while the default password is still in force a session may reach
  only the auth endpoints (including the video/input WebSocket upgrade), so the
  device cannot be driven over the wire before a real password is set.

### Fixed
- ATX: a settings change can no longer reset a GPIO out from under a button
  press in progress (a dedicated operation lock serialises the two).
- The failed-login counter is now read and updated under the lock.
- Virtual media: the rescue-write flag is flipped only under the media lock on
  every path.

## [0.12.0] - 2026-07-29

### Added
- A "Restart device" button in Settings, so a reboot no longer needs a
  power-cycle or a curl call.

### Changed
- The H.264 path runs its colour conversion (PPA) and the encode on two tasks
  over two YUV buffers, so one frame encodes while the next converts. Measured
  on hardware this is a smaller win than hoped (~6.7 -> ~7.3 fps at 1080p): the
  bottleneck is PSRAM bandwidth, not serialised compute - the conversion moves
  ~9 MB per frame and running it alongside the encode makes the two contend for
  the memory bus. The real fix (feeding the encoder YUV directly, without the
  conversion pass) is a larger change tracked separately.

## [0.11.0] - 2026-07-29

### Fixed
- The WebSocket endpoints (`/video`, `/ws`) returned 404, so nothing that rides
  them worked in a browser - no H.264 video, no keyboard or mouse - even though
  every REST route was fine. The HTTP server's handler table was one size too
  small for the number of routes (the ATX endpoints added in 0.9.0 pushed it
  over), and `/video` and `/ws` are registered last, so they were the ones
  silently dropped. Raised the limit with headroom. This is why H.264 and input
  had stopped working in the browser since 0.9.0.
- The "JPEG quality" setting no longer shows "not probed yet" when the device
  booted on the H.264 codec: MJPEG is now probed at start-up like H.264, rather
  than only when its encoder first opens.

### Added
- The device is its own certificate authority. It generates a small root CA once
  (kept in NVS) and signs its server certificate with it; the CA is offered at
  `/cert.pem` (also on the plain port-80 server) and from Settings -> Security.
  Importing the CA into a client's trust store is what a self-signed *leaf* could
  never do - it makes the device genuinely trusted, which clears the browser
  warning and, being a secure context, enables the WebSocket channel and the
  H.264 decoder. The leaf is re-issued under the same CA when the hostname or a
  static IP changes, so the one-time import survives.

### Changed
- `/api/v1/video/status` reports the PPA colour-conversion time (`ppaUs`)
  separately from the encode time (`encodeUs`) on the H.264 path. Measured on
  hardware this showed the conversion, not the encoder, is the bottleneck
  (~104 ms vs ~45 ms at 1080p) - the encoder-load figure now covers both stages.

## [0.10.1] - 2026-07-29

### Changed
- The self-signed certificate now lists a configured static IP as a
  subjectAltName, so reaching the device by address no longer adds a name
  mismatch on top of the self-signed warning. The certificate is keyed by
  hostname and static IP together and regenerates when either changes; a device
  on DHCP is unaffected (its address can move, so it stays hostname-only) and
  keeps its certificate across the upgrade.

## [0.10.0] - 2026-07-28

### Added
- Touch mode: on a phone or tablet the screen becomes a trackpad instead of
  fighting the desktop pointer mapping. One finger moves the pointer, tap is a
  left click, two-finger tap a right click, two-finger drag scrolls, and a long
  press then drag holds the button. An on-screen keyboard types through the
  target's own layout, like paste does. Auto-detected from a coarse pointer,
  with a manual "Touch" toggle in the action bar.
- A clear "another session is in control" state. The device now grants control
  to the first client and holds it there, rather than letting each new frame
  silently steal the session; a second viewer sees a banner with a "Take
  control" button instead of dead input, and taking control demotes the previous
  holder to a viewer rather than disconnecting it.

### Changed
- The control WebSocket protocol gained a "take control" client message (0x07)
  and a "control state" device message (0x83); the console polls it so a viewer
  notices when the session is freed or taken over.

### Changed
- Numeric settings (GPIO pins, pulse lengths, thresholds) are now plain number
  fields instead of sliders - easier to set a value precisely.
- The Power panel hides its controls entirely when ATX control is switched off,
  rather than showing a disabled hint.
- The ATX wiring guide (`docs/wiring.md`) is board-agnostic, with a wiring
  diagram (`docs/atx-wiring.svg`) and only illustrative header examples.

## [0.9.0] - 2026-07-28

### Added
- ATX power control: the device can "press" the target's front-panel power and
  reset buttons through optocouplers, and read its power LED back the same way.
  A PC817 two-channel module on each side does the whole job with no custom
  board - see `docs/wiring.md`. New endpoints `POST /api/v1/power/click`,
  `/api/v1/power/hold` (a five-second hard off) and `/api/v1/power/reset`; the
  target's power state (when a LED is wired) appears in `/api/v1/system/info`
  under `atx`. A Power panel in the console drives it, with confirmation on the
  destructive actions.
- The GPIO pins, pulse lengths and drive/sense polarity are all runtime
  settings (Settings -> Power), so the same firmware runs on a board with no ATX
  wiring - it simply reports the capability unavailable with the reason - and a
  wrong guess about a module's trigger polarity is a checkbox, not a reflash.

Not yet verified against a real optocoupler: the firmware path (capability,
endpoints, GPIO pulsing, LED sense) is exercised on hardware, but the button
polarity and LED sensing are confirmed only once a module is wired.

## [0.8.0] - 2026-07-28

### Added
- Connection icons in the action bar - HDMI in, USB to the target, microSD and
  Ethernet - each lit by its live state (green when active, amber when the HDMI
  cable is up but there is no picture, dim when nothing is connected), so a
  glance shows what is plugged in.
- Ethernet link state (up/down and negotiated speed) is reported in
  `/api/v1/system/info` under `net`, and drives the Ethernet connection icon.

### Changed
- The status bar adapts to phones: on a narrow screen the secondary video stats
  (codec/fps, skipped frames, bitrate, encoder load) are hidden, keeping the
  online state, resolution, the version widget and the theme toggle; the full
  figures remain in the Video panel.
- The web console now lives in its own repository
  ([espkvm/console](https://github.com/espkvm/console)) and is pulled in as a
  git submodule at `web/`. Clone with `--recursive`; see the README.

## [0.7.0] - 2026-07-26

### Added
- Build target for the Espressif ESP32-P4 Function EV Board (chip rev v3.2,
  16 MB flash) as an sdkconfig overlay: `boards/funcev_p4.defaults` +
  `partitions_funcev.csv`, documented in `boards/README.md`. Chip rev <3.0 and
  >=3.0 are mutually exclusive targets, so boards are built separately; the
  default `idf.py build` still targets the Waveshare ESP32-P4-ETH (rev v1.3).
- CI now builds every board and publishes each one's release assets (the `.bin`
  filenames carry the board name) and its own Pages manifests under
  `firmware/<board>/` and `flash/<board>/`. The root `firmware/` and `flash/`
  still mirror the Waveshare board, so devices and flasher pages that predate the
  per-board layout keep working unchanged.
- The default update-manifest URL is now a build option (`KVM_UPDATE_URL`) so a
  board's build points its update check at its own manifest. The Waveshare
  default is unchanged (the root manifest); the Function EV build points at
  `firmware/funcev/`, so it never tries to install a rev <3.0 image.

### Fixed
- The "always powered" microSD configuration (`KVM_SD_PWR_GPIO=-1`, documented
  in PORTING.md) did not actually compile - a constant negative shift tripped
  `-Werror=shift-count-negative`. That path now builds.

## [0.6.0] - 2026-07-25

### Changed
- The firmware version is now a widget in the status bar: an outlined badge that
  shows a dot when a newer build is published and whose outline fills as a
  progress ring while an update installs. Clicking it opens the firmware panel -
  the update check, installing a release or a hand-picked `.bin` - which moved
  out of Settings so the whole flow is one click from the version.
- Live diagnostics (chip temperature, memory, uptime, ESP-IDF version) moved from
  the Settings System tab into a rail button above Settings: the button shows the
  temperature and a coarse uptime (55min, 1h, 3d, 2w, 5m, 1y), the rest opens in
  a popup beside the rail.
- The guessed target OS moved out of the action bar into its own rail button
  above Settings (next to the diagnostics one); clicking it shows the raw USB
  fingerprint. The popups open toward the stage on whichever side the rail is on.
- The image list, active-medium choice and uploads moved out of Settings into the
  Virtual media panel itself; Settings keeps only the media settings.
- The button rail and its panels now open on the same side, and a `Panel side`
  setting (Settings -> System) flips the whole unit left or right.

## [0.5.1] - 2026-07-25

### Added
- A progress bar while an image uploads - a firmware update, a microSD image or
  the on-flash rescue image - so it is clear the upload is moving, not stalled.

## [0.5.0] - 2026-07-25

### Added
- **User macros.** Save named key macros - a short script of key chords, typed
  text and delays (`key ctrl+alt+f2`, `type root`, `delay 500`) - in the Input
  panel and replay them with one click, for a fixed sequence like stepping
  through a BIOS or an unattended install. Stored on the device, so they follow
  it rather than the browser.

### Changed
- The virtual-media tab now points at netboot.xyz for where to get a rescue
  image, since the browser cannot fetch one cross-origin to install in one click.

## [0.4.0] - 2026-07-25

### Added
- **Wake-on-LAN.** The Power panel gains a Wake button that sends a magic packet
  to the target's MAC (set under Settings -> Power), to power on a machine that
  keeps standby power and has WoL enabled - no ATX wiring needed.
- **Static network addressing.** The Network tab's static IP, mask, gateway and
  DNS now take effect; DHCP stays the default. A malformed address falls back to
  DHCP at boot so a typo cannot strand the device, and the board button now
  reverts to DHCP alongside clearing the password, so a valid-but-wrong address
  is recoverable the same way a forgotten password is. (The hostname / mDNS name
  already applied.)

## [0.3.0] - 2026-07-24

### Fixed
- A network firmware update could come up reachable and then be rolled back on
  the next reset. The image was confirmed only after every peripheral had
  started, so a USB or capture block left in a bad state by the warm restart
  kept the confirmation from being reached. The image is now confirmed the
  moment the network and web server answer - the point at which the device is
  re-flashable - and the warm-reset-prone peripherals start afterwards, where a
  failure degrades the device instead of reverting it. Boot phases are logged
  so a future failure is diagnosable from the serial console.

### Added
- **Target-OS awareness.** The device guesses whether the target is Windows,
  macOS, Linux or Android from how it enumerates USB, and shows the guess (with
  the raw fingerprint) next to the USB status and in Settings. A `Target OS`
  setting overrides it when the guess is wrong or unknown.
- The console tailors input to that OS: it labels the Meta key Win, Cmd or
  Super, and offers OS-specific key combinations - the Linux magic-SysRq
  sequences **REISUB** (safely reboot a hung machine) and **REISUO** (safely
  power it off), and **Ctrl+Alt+F1-F6** to switch virtual terminals.
- **Built-in rescue media.** A small bootable image - iPXE, memtest, a DOS
  floppy - can be kept in a 4 MB flash partition and served to the target over
  the same USB drive as the card's files, with no microSD needed. Unlike the
  card, it can be written from the console, because this board's flash writes are
  reliable where its SD writes are not. Upload it in the virtual-media tab and
  pick it as the active medium; the card and the rescue image coexist.

### Changed
- The set of USB functions is now built into the descriptor at start-up instead
  of being fixed, so mass storage can be left out to free its endpoints (room a
  future USB-network interface can use). **Expose virtual media** now controls
  whether the USB drive is present at all and takes effect after a restart; with
  it off the device is a plain keyboard and mouse. Swapping the image once the
  drive exists still takes effect immediately.
- The partition table gains a 4 MB `rescue` partition, appended so every existing
  offset is unchanged. A device adopts it with a one-time full flash over cable
  (the browser flasher works); it cannot be taken on by an over-the-network
  update.

### Security
- Bumped the build-time dev dependencies (Vite, esbuild) to clear advisories.
  These are toolchain packages that never ship in the firmware, and the issues
  were dev-server-only, so there is no effect on the device - the update just
  keeps the tree clean.

## [0.2.1] - 2026-07-24

### Changed
- Board wiring is now configured in `menuconfig` instead of a hardcoded header:
  the microSD pins and slot power-gate, the TC358743 I2C pins and reference
  clock, and the BOOT button GPIO joined the Ethernet pins that were already
  there, all with the Waveshare ESP32-P4-ETH values as defaults. Porting to
  another ESP32-P4 board is now a menuconfig edit; set the button or SD
  power-gate GPIO to `-1` on a board that lacks them. No behaviour change on
  the reference board - the binary is identical. See `docs/PORTING.md`.

### Added
- `docs/PORTING.md` - adapting the firmware to another ESP32-P4 board.
- `AGENTS.md` - repo conventions, build/flash commands and hard-won gotchas,
  for contributors and AI coding agents.

## [0.2.0] - 2026-07-24

### Added
- **Virtual media.** A disk image on the microSD card is presented to the target
  as a read-only USB drive it can boot from - a rescue system, an installer, a
  live image. The console gains a media tab to list the images on the card and
  choose which one the target sees.

### Notes / known limits
- The card is served **read-only**: this board cannot write the microSD
  reliably, so images are prepared in an external card reader. Upload and delete
  in the console are disabled with the device's own explanation.
- The card must be **FAT32**; a single image is capped at 4 GB (a FAT32 limit).
- microSD reads run at 4 MHz (~1.5 MB/s). Higher clocks fail every multi-block
  read on this board - a known ESP32-P4 SD limitation - so booting a heavy
  graphical image is slow (minutes); a minimal rescue image boots in about a
  minute. The card mount is retried at boot; a marginal card may need reseating.

## [0.1.2] - 2026-07-23

Foundation releases. The core KVM: HDMI capture with automatic mode following,
MJPEG and hardware H.264 streaming, an absolute and relative USB pointer and a
full keyboard, the Vue web console, HTTPS with a self-signed certificate,
login with a physical password reset, thermal protection, and firmware update
over the network with rollback. microSD is mounted and reported (the base the
virtual-media feature above builds on).

## [0.1.1] - 2026-07-23

First tagged build: the release pipeline (GitHub Actions), the update manifest
on GitHub Pages, and the browser flasher.
