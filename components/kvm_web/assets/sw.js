/*
 * Minimal service worker - present so the console is an installable PWA (runs
 * full-screen, standalone, with a home-screen icon). A KVM is useless offline,
 * so this deliberately does NOT try to cache live data: the video /stream, the
 * /ws control channel and every /api call always go straight to the device.
 * It only caches the app shell ("/") so a launch survives a momentary network
 * blip and shows the UI (which then reports the device as unreachable) instead
 * of the browser's dinosaur.
 */
const CACHE = "espkvm-shell-v1";

self.addEventListener("install", (e) => {
  e.waitUntil(
    caches
      .open(CACHE)
      .then((c) => c.add("/"))
      .then(() => self.skipWaiting()),
  );
});

self.addEventListener("activate", (e) => {
  e.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

self.addEventListener("fetch", (e) => {
  /* Only the top-level navigation falls back to the cached shell. Everything
     else (stream, websocket, api, assets) must reach the device live. */
  if (e.request.mode === "navigate") {
    e.respondWith(fetch(e.request).catch(() => caches.match("/")));
  }
});
