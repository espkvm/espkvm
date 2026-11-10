# Notifications

The device can say something happened while nobody was watching: a watched
phrase appeared on the screen, the screen went blank, a dashcam clip was saved,
the tailnet key is about to run out. It sends that to Telegram, to a webhook,
and to browsers (Web Push), in any mix. Everything here is set in the console under Settings &rarr;
Notifications; `POST /api/v1/notify/test` sends a test one and
`GET /api/v1/notify/status` says how the last one went.

## What goes where

Each channel takes the kinds of event its column in the table under Settings
&rarr; Notifications ticks: a watched phrase on the screen, a blank screen, a
screen area, a dashcam clip, a target log alert, the Tailscale key running out.
Everything but the blank screen is ticked to start with. A test, and the note
that alerts were dropped, go to every channel.

In the settings API each column is an int, a bit per row in that order:
`notify_tg_ev`, `notify_hook_ev`, `notify_push_ev` (bit 0 the watched phrase,
bit 5 the Tailscale key). A new channel is a new column, with a file of its own
in `components/kvm_notify/`.

## Telegram

Put a bot token and a chat id in the settings. **Find chats** lists the chats
the bot can already write to, so the id does not have to be looked up by hand.
A screenshot is attached on either codec. A dashcam clip under
50 MB is uploaded as a video that plays in the chat; a bigger one is named
instead.

## Browsers (Web Push)

**Send them to this browser** in Settings &rarr; Notifications asks the browser
for permission and subscribes it. From then on alerts show up like any app's,
also with the console closed. Chrome, Edge, Firefox and Safari do it. On an
iPhone or iPad the console has to be added to the Home Screen first and opened
from there. The console must be on HTTPS with a certificate the browser trusts.

Nothing has to be registered anywhere. The device makes its own VAPID key on
first use and signs each message with it. The message is encrypted for that
one browser and goes to the browser's push service (Google's for Chrome and
Edge, Mozilla's for Firefox, Apple's for Safari), which wakes the browser. So
the device needs a way out to the internet, the same as for Telegram.

Up to 8 browsers. The list under the button has a **Remove** for each. A
browser that unsubscribed on its own is dropped at the next send. No
screenshot or log goes this way: a push message is a few hundred bytes.

| Call | What it does |
| --- | --- |
| `GET /api/v1/push` | the VAPID public key and the browsers |
| `POST /api/v1/push/subscribe` | `{endpoint, keys: {p256dh, auth}, name}`, as `PushSubscription.toJSON()` gives it |
| `POST /api/v1/push/remove` | `{id}` or `{endpoint}` |

## Webhook

A plain `POST` to the URL in the settings, `Content-Type: application/json`:

```json
{
  "title": "Screen text",
  "message": "The phrase \"Press F1 to continue\" is on the screen.",
  "device": "espkvm",
  "at": 1790857313,
  "log": "the last few kilobytes of the device log"
}
```

| Field | Always there | What it is |
| --- | --- | --- |
| `title` | yes | what kind of event it is, a few words |
| `message` | yes | the sentence a person reads |
| `device` | yes | the device's hostname, so several devices can share one endpoint |
| `at` | no | when it happened, in Unix seconds - there once the device's clock is set |
| `log` | no | the tail of the device log, only while **Attach the log tail** is on |

Nothing is uploaded to a webhook: a clip is named in `message` as the file on
the card ("... - saved on the card as VIDEO/20260917-140322-event.mp4"), and a
screenshot is not attached at all. Any 2xx answer counts as delivered; anything
else is reported in `GET /api/v1/notify/status`.

The body is small enough for the usual receivers - an n8n or Node-RED webhook
node, a Home Assistant `webhook` trigger, an ntfy or Gotify endpoint behind a
small script, a Slack or Discord relay. There is no signature or shared secret,
so put the endpoint somewhere only your network can reach it, or give the URL a
long unguessable path.

## When the network is down

An alert that cannot go out - no network, a timeout, the server busy - waits
and goes out when the device can reach it again. It is retried oldest first,
after 15 seconds and then less and less often, up to every 5 minutes. A new
event tries the waiting ones again at once.

The screenshot and the log tail are taken when the event happens, not when it
is finally sent, so a late message shows the screen as it was. One sent more
than a minute late says when it happened: "(Happened at 03:12:05, 28 min ago -
the device could not send it then.)" A webhook gets the same in `at`.

With a microSD card the device can write to, each waiting alert is a few files
in `.notify/` on the card - what it says, its screenshot and its log tail - so
the list survives a restart, and the screenshots do not sit in memory. After a
restart the rest go out with "(Happened at ..., before the device restarted.)".
Without a writable card (none, handed to the target, or a board that cannot
write to it) they wait in memory and a restart loses them.

Up to 50 alerts wait; past that the oldest gives way, and once the rest are
through one more message says how many were dropped. In memory, the
screenshots share at most 2 MB, and one is only kept while at least 3 MB of
PSRAM stays free - otherwise the alert goes as text. A refusal - a wrong token
or chat id, a 4xx from the webhook - is not retried. Telegram and the webhook
are counted apart: if one went through, only the other is tried again.
`GET /api/v1/notify/status` shows how many are waiting in `pending`. Switching
notifications off clears the list, on the card too.

## The tailnet key

Tailscale gives a node key six months at most, and when it lapses the device
drops off the tailnet until someone authorises it again. The device learns the
date from the control plane, shows it in Settings &rarr; VPN, reports it in
`GET /api/v1/system/info` as `ts.keyExpiry` (Unix epoch seconds, 0 when the
tailnet has key expiry switched off), and sends a notification a set number of
days before - **Warn before the key runs out** in the VPN settings, 14 days by
default, 0 to switch it off. The warning needs the clock in sync, which is what
the time settings under System are for.
