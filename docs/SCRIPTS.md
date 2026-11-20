# Macros and runbooks: the script language

A macro and a runbook are written in the same language: one command per line,
`#` starts a comment, blank lines are ignored, case does not matter for the
commands and the key names.

The difference is where they run. A **macro** is played by the console in your
browser, through the same path as your own keystrokes, and stops if the tab
closes. A **runbook** is run by the device itself: it can wait for words on the
screen, and it carries on with the browser closed. Every command a macro knows,
a runbook knows too.

## Commands

| | |
|---|---|
| `key <chord>` | press the chord and let go: `key f2`, `key ctrl+alt+del`, `key shift+tab` |
| `type <text>` | type the rest of the line as characters |
| `delay <ms>` | pause for that many milliseconds |

Runbooks add these:

| | |
|---|---|
| `wait <phrase>` | hold until a row of the screen contains the phrase |
| `gone <phrase>` | hold until no row contains it |
| `timeout <seconds>` | how long the waits below it may take; 60 if unsaid |
| `record` | start recording the screen to the microSD card, and go on to the next line |
| `record <seconds>` | the same, stopping by itself after that long |
| `record stop` | stop the recording or the timelapse |
| `timelapse <every>` | record one frame every that many seconds, played back at 25 fps, until stopped |
| `timelapse <every> <seconds>` | the same, stopping by itself after that long |
| `screenshot` | save the screen as a JPEG on the microSD card |
| `hdmi standby`, `hdmi wake` | put the HDMI source to standby, or wake it, over HDMI-CEC |
| `hdmi key <name>` | send a remote-control key over HDMI-CEC: `up`, `down`, `left`, `right`, `select`, `back`, `home`, `menu`, `play`, `pause`, `stop`, `volume_up`, `volume_down`, `mute`, `0` .. `9`, `red`, `green`, `yellow`, `blue` and more |

A recording started by a runbook carries on after the runbook ends, until
`record stop`, its length, or the length limit in Settings. A timelapse has no
length limit unless it is given one. If a recording or
a screenshot cannot be made - no card, H.264 not selected - the runbook stops
there and says why.

A wait that runs out stops the runbook at that line, and the panel says so.
The device reads the screen as characters, so a wait only ever sees a text
screen - a BIOS, a boot menu, a boot loader, a console. On a picture, `wait`
runs out and `gone` is satisfied at once. Phrases are matched within one row,
case is ignored, and a phrase is at most 63 ASCII characters.

## Chords

A chord is one key and any number of modifiers, joined with `+`.

Modifiers: `ctrl` (or `control`), `shift`, `alt` (or `option`), `gui` (or
`win`, `cmd`, `super`, `meta`).

Keys:

| | |
|---|---|
| letters and digits | `a` .. `z`, `0` .. `9` |
| function keys | `f1` .. `f12` |
| `enter` / `return`, `esc` / `escape`, `tab`, `space`, `backspace` | |
| `up`, `down`, `left`, `right` | the arrows |
| `home`, `end`, `pageup` / `pgup`, `pagedown` / `pgdn` | |
| `insert` / `ins`, `delete` / `del` | |
| `minus`, `equal` | the `-` and `=` keys |
| `capslock`, `scrolllock`, `pause`, `printscreen` / `prtsc` / `sysrq` | |

The key is a position, not a character: `key a` presses the key that is `a` on
a US keyboard, whatever the target's layout says it is.

A modifier on its own is a keypress too: `key gui` taps the Windows key,
`key ctrl+shift` holds and releases both.

## DuckyScript

You can also paste a Hak5 DuckyScript and it runs as-is - the same payloads that
target a Rubber Ducky. It is recognised by its upper-case verbs, so no switch is
needed; put it in the same box.

| | |
|---|---|
| `REM <text>` | a comment |
| `STRING <text>` | type the text |
| `STRINGLN <text>` | type the text and press Enter |
| `DELAY <ms>` | pause |
| `DEFAULT_DELAY <ms>` | pause that much after every command below it |
| `REPEAT <n>` | do the line before this one n more times |
| `GUI r`, `CTRL ALT DELETE`, `ENTER`, `F2`, `DOWN` | a chord or a key: modifiers (`GUI`/`WINDOWS`, `CTRL`, `ALT`, `SHIFT`) and one key, space-separated |

Only the keyboard is covered; mouse and `WAIT_FOR_BUTTON_PRESS` and the like are
not. `STRING` is US ASCII, like `type`. To wait for the screen, use a runbook in
our own language - DuckyScript has no equivalent.

## Typing

`type` is different in the two places. A macro sends each character through
the keyboard layout chosen in Settings -> Input (and the Target OS, where
Windows and Linux place a character differently), so it can type what that
layout can. A runbook runs on the device, which has no layout tables: it types
printable US ASCII only, and the editor refuses a line with anything else.

## Limits

A runbook has at most 64 steps, a `delay` is 1 to 60000 ms, a `timeout` 1 to
3600 s, a `record` 1 to 86400 s. All the runbooks together must fit in 3000 bytes, all the macros in
2000; the editor says when they do not.

## Schedules

The Automation panel can also run a runbook, or a single action, on a
timetable. A schedule is a five-field cron line and an action:

    minute hour day-of-month month weekday

Each field is `*`, a number, a range `a-b`, a step `*/n`, or a comma list.
`0 7 * * 1-5` is 07:00 on weekdays; `*/15 * * * *` is every fifteen minutes;
`0 2 1 * *` is 02:00 on the first of the month. When both day-of-month and
weekday are set, either one matching fires it.

The action is Wake-on-LAN, one of the ATX buttons (power, reset, force-off), a
named runbook, or a restart of the device itself.

Schedules need the wall clock, which the device sets over SNTP - point it at a
time server on the local network if there is no internet. Until the clock is
set, nothing fires and the panel says so. Times are in the time zone set in
Settings, System: pick a city from the list there. Underneath it is a
POSIX TZ string (`UTC0`, `GMT0BST,M3.5.0/1,M10.5.0`, `MSK-3`), which the API takes
as `sched_tz`.

## Examples

Into the setup of a machine that says so on its splash screen:

```
timeout 120
wait Press F2
key f2
wait Boot
```

Log in on a Linux console and start a service:

```
wait login:
type root
key enter
delay 500
wait assword
type hunter2
key enter
wait #
type systemctl start nginx
key enter
```

Restart a machine and keep a video of how its boot went, with a picture of the
setup screen:

```
record 180
key ctrl+alt+del
timeout 120
wait Press F2
key f2
wait Boot
screenshot
```

Pick the second entry of a boot menu, as a macro or a runbook:

```
key down
delay 200
key enter
```
