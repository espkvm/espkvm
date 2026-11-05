# Setting it up

What comes after the [Quick start](../README.md#quick-start): trusting the
device, certificates, the network, and the first sign-in.

## HTTPS and the device's certificate

The device is its own certificate authority. It makes a CA on first boot and
signs its own certificate with it, so the browser warns you the first time.
Clicking through that warning is enough to sign in and watch the MJPEG stream.

H.264, the keyboard and the mouse need more: a real *secure context*, which a
self-signed certificate does not give until it is trusted. Without it the browser
runs neither the WebSocket channel nor the H.264 decoder.

To clear the warning for good, trust the device's CA once. Download it from
**Settings -> Security -> Download CA certificate**, or from
`http://espkvm.local/cert.pem` - that one is served in the clear, so you can
fetch it before trusting anything. Import it into your operating system or
browser as a **trusted authority**, not under "your certificates". Then reach the
device by name, at **https://espkvm.local**.

On a static IP the certificate names the address as well, so `https://<ip>` works
once the CA is in. On DHCP, use the name.

## Your own certificate

A certificate from an internal CA, or a real one for a name that
resolves to the device - goes in under **Settings -> Security -> Device
certificate**: the chain (leaf first) followed by its private key. Renewing it
is the same form: the new one replaces the old in place, no need to go back to
the self-signed one first. A renewal hook can do it with no browser:

```sh
# sign in once, then send the chain and the key as one PEM body
curl -sk -c jar -H 'Content-Type: application/json' \
  -d '{"user":"admin","password":"..."}' https://kvm.example.com/api/v1/auth/login
cat fullchain.pem privkey.pem | curl -sk -b jar -H 'X-ESP-KVM: 1' \
  -X PUT --data-binary @- https://kvm.example.com/api/v1/tls/cert
```

The device restarts to apply it. Going back to the self-signed certificate
leaves a browser that does not trust the device's CA refusing it - quietly: the
console's service worker serves its saved copy, which can only say "Cannot
reach the device", even after the browser restarts (a private window works,
having no saved copy). That page now has "Open without the saved copy", which
drops it and shows the browser's certificate warning.

## Let's Encrypt over Tailscale

Turn on MagicDNS and HTTPS certificates in
the Tailscale admin console (the DNS page), then **Settings -> VPN -> Trusted
certificate (Let's Encrypt)**. The device asks Let's Encrypt for a certificate
for its tailnet name; Tailscale publishes the DNS record that proves the name is
yours, so no domain, DNS token or open port is needed. It renews after two
thirds of the certificate's life, and retries after 1, 2, 4 and 8 hours if
something fails. Progress and errors show in **Settings -> Security -> Device
certificate**. The certificate is only served to browsers that ask for the
tailnet name: `espkvm.local` and the IP addresses keep the device's own, and
Let's Encrypt never issues for a private IP. The name ends up in public
Certificate Transparency logs.

## IPv6

The device is dual-stack. On a network that advertises IPv6 it takes an address
there too, with nothing to configure, and `espkvm.local` resolves to it. From the
next restart the certificate names those addresses, so `https://[address]/` is
trusted like the v4 one. There is a switch in **Settings -> Network -> IPv6**.

Tapping the **network icon** in the console footer shows the whole picture: what
the device is connected by, the name it answers to, its IPv4 address, every IPv6
address with what each is good for, and the MAC for a DHCP reservation. Each one
is a link and has a Copy button.

A network with no IPv4 at all works too. The one thing that cannot follow is
Wake-on-LAN - the magic packet is an IPv4 broadcast, and IPv6 has none - so the
console says it is unavailable instead of pretending. The WireGuard and Tailscale
tunnels still need a peer reachable over IPv4.

## The first sign-in

Sign in as **admin / admin**. The console will not go any further until that
password is changed: a KVM left on the password it shipped with is a keyboard
on someone else's machine, offered to whoever finds it.

## No cable: the setup hotspot

If there is no cable at hand and the board carries a WiFi co-processor, a new
device puts out a hotspot of its own instead. It waits twenty seconds for the
network port, and if nothing turns up it opens **ESP-KVM-xxxx** with no
password - join it and the console is at `http://192.168.4.1/`. A board with no
network port at all (the ESP32-P4-WIFI6, the FireBeetle 2) opens it straight
away. It is open
because the password it would otherwise invent is printed to a serial console
and a display, and some of these boards have neither. Nothing is handed over
with it: until a real password is set, the only thing that answers is the page
that sets one. There is a switch in **Settings -> Network** to turn it off.

Setting the password closes the open hotspot, so the same form asks where the
device goes next: the network cable (on boards that have one), your WiFi, or
its own hotspot with a password you choose. The device then restarts into it.

## Updates

After that the cable is only needed if something goes badly wrong - updates are
installed from the console itself. Checking for them is **off by default**:
turn on **Settings -> System -> Updates -> Offer firmware updates**, and the
console tells you when a newer build is out and installs it in one click. Your
browser asks, not the device, so the device still never reaches the internet.
With the check on, the browser also tells check.espkvm.io once a day a random id
of the device (not taken from the hardware; a settings reset makes a new one),
its board, its version and the chip revision. That is how I count the devices in use. Nothing else
is sent. The code that counts them is in
[espkvm/check](https://github.com/espkvm/check).

## Reaching it from outside: Tailscale or WireGuard

There are two VPNs built in, in Settings &rarr; VPN. Tailscale needs no port forward, no gateway and no VPS: the device joins your
tailnet and answers at its 100.x address, or its MagicDNS name, from anywhere.
Its certificate is valid there too. With **Trusted certificate (Let's Encrypt)**
on, the device also gets a Let's Encrypt certificate for its tailnet name
(`espkvm.tailnet.ts.net`), so any browser on the tailnet trusts it with nothing
imported. WireGuard is a split tunnel - it makes its
own key on the device and shows the public half in the VPN tab, to register on
your hub.

Tailscale does not need an auth key. Turn it on with the key field empty and the VPN tab shows **Sign in to Tailscale**: the link opens Tailscale's page, you approve the device with your account, and it joins a few seconds later. An auth key still works the same way as before, for a device you set up without a browser. **Log out of the tailnet** makes the device forget its keys, so the next start asks again.

The same tab lists the other devices on the tailnet: whose they are, whether traffic to each goes straight or through a Tailscale relay (and which one), and the last round trip. A slow console over Tailscale is usually a relay. The list of open consoles names a tailnet device and its owner instead of a 100.x address.

Two settings use what Tailscale knows about who is connecting:

- **Console only over Tailscale**: while the device is on the tailnet, the console answers only to tailnet addresses. The setup hotspot still works, and when Tailscale is down the console answers on the LAN again, so a broken tailnet does not lock you out.
- **Sign in these tailnet users** and **Let these tailnet users watch**: Tailscale logins (`alice@example.com`), separated by commas. A browser on one of their devices is signed in as the owner, or as the viewer, with no password and no code. This works only after a password of your own is set.
