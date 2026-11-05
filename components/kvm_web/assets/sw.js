/*
 * Minimal service worker - present so the console is an installable PWA (runs
 * full-screen, standalone, with a home-screen icon). A KVM is useless offline,
 * so this deliberately does NOT try to cache live data: the video /stream, the
 * /ws control channel and every /api call always go straight to the device.
 * It only caches the app shell ("/") so a launch survives a momentary network
 * blip and shows the UI (which then reports the device as unreachable) instead
 * of the browser's dinosaur.
 */
/* v2: the copy is refreshed on every load (v1 kept the one from install day,
 * which could be months older than the firmware). Renaming it also makes the
 * browser reinstall this worker and drop the old copy. */
const CACHE = "espkvm-shell-v2";

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
    e.respondWith(
      fetch(e.request)
        .then((resp) => {
          /* Keep the copy in step with the firmware: the next fallback shows
             the console this device is running now. */
          if (resp.ok) {
            const copy = resp.clone();
            caches.open(CACHE).then((c) => c.put("/", copy));
          }
          return resp;
        })
        /* Also what a refused certificate looks like from here - the browser
           shows no warning to a worker - so the copy says what to do then. */
        .catch(() => caches.match("/")),
    );
  }
});

/* Web Push from the device (Settings, Notifications, "This browser"). The
 * device sends {title, body, device, at}; shown even with no console open. */
self.addEventListener("push", (e) => {
  let m = {};
  try {
    m = e.data ? e.data.json() : {};
  } catch {
    m = { body: e.data ? e.data.text() : "" };
  }
  const title = m.device ? `${m.device}: ${m.title || "ESP-KVM"}` : m.title || "ESP-KVM";
  e.waitUntil(
    self.registration.showNotification(title, {
      body: m.body || "",
      icon: "/icon-192.png",
      timestamp: m.at ? m.at * 1000 : Date.now(),
      data: { url: "/" },
    }),
  );
});

/* A click opens the console, or brings an open one to the front. */
self.addEventListener("notificationclick", (e) => {
  e.notification.close();
  e.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((list) => {
      for (const c of list) {
        if ("focus" in c) return c.focus();
      }
      return self.clients.openWindow("/");
    }),
  );
});
