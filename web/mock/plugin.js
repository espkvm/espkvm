/*
 * A stand-in for the device, so the interface can be built and looked at
 * without hardware attached.
 *
 * It answers the same endpoints as the firmware and pushes the same control
 * frames, which means every state the UI has to handle - no signal, no target
 * attached, a resolution change, a capability the hardware lacks - can be
 * produced on demand instead of waited for. Several of them are awkward to
 * reproduce on real hardware and would otherwise go untested until a user
 * found them.
 *
 * Enable with:  npm run dev:mock
 */

import { WebSocketServer } from "ws";

import { renderFrame } from "./frame.js";

/* Mirrors components/kvm_config/kvm_settings_table.c. */
const SCHEMA = [
  { key: "vid_codec", section: "video", title: "Stream codec", type: "enum",
    choices: ["mjpeg", "h264"], default: 0,
    help: "H.264 needs roughly a tenth of the bandwidth of MJPEG but requires a browser with WebCodecs." },
  { key: "jpg_quality", section: "video", title: "JPEG quality", type: "int", min: 1, max: 100,
    default: 70, requires: "mjpeg", help: "Higher is sharper and larger. Only affects the MJPEG codec." },
  { key: "h264_kbps", section: "video", title: "H.264 bitrate (kbit/s)", type: "int", min: 500,
    max: 20000, default: 4000, requires: "h264" },
  { key: "vid_fps_max", section: "video", title: "Frame rate limit", type: "int", min: 1, max: 60,
    default: 30, requires: "video" },
  { key: "vid_adapt", section: "video", title: "Skip unchanged frames", type: "bool", default: 1,
    requires: "video",
    help: "Stop encoding while the target's screen is static. Drops the bitrate to nearly zero on an idle desktop." },
  { key: "edid_prof", section: "video", title: "EDID profile", type: "enum",
    choices: ["full", "1080p30", "custom"], default: 0, requires: "video", reboot: true,
    help: "What the capture card claims to be." },

  { key: "ptr_engage", section: "input", title: "Start controlling on", type: "enum",
    choices: ["click", "hover"], default: 0, requires: "hid",
    help: "\"click\" waits for a click on the video before input reaches the target." },
  { key: "mouse_mode", section: "input", title: "Pointer mode", type: "enum",
    choices: ["absolute", "relative", "auto"], default: 0, requires: "hid" },
  { key: "mouse_sens", section: "input", title: "Relative sensitivity (%)", type: "int", min: 10,
    max: 400, default: 100, requires: "hid" },
  { key: "scroll_inv", section: "input", title: "Invert scroll wheel", type: "bool", default: 0,
    requires: "hid" },
  { key: "kbd_layout", section: "input", title: "Target keyboard layout", type: "enum",
    choices: ["en_us", "ru_ru"], default: 0, requires: "hid",
    help: "A KVM sends key positions, not characters." },
  { key: "type_delay", section: "input", title: "Paste keystroke delay (ms)", type: "int", min: 1,
    max: 200, default: 8, requires: "hid" },

  { key: "msc_enable", section: "storage", title: "Expose virtual media", type: "bool", default: 0,
    requires: "msc" },
  { key: "msc_mode", section: "storage", title: "Media type", type: "enum",
    choices: ["cdrom", "removable", "whole_sd"], default: 0, requires: "msc" },
  { key: "msc_image", section: "storage", title: "Mounted image", type: "string", maxLength: 63,
    default: "", requires: "msc" },

  { key: "atx_enable", section: "power", title: "Enable ATX control", type: "bool", default: 0,
    requires: "atx" },
  { key: "atx_long_ms", section: "power", title: "Force-off hold (ms)", type: "int", min: 1000,
    max: 15000, default: 5000, requires: "atx" },

  { key: "net_hostname", section: "network", title: "Hostname", type: "string", maxLength: 31,
    default: "espkvm", reboot: true },
  { key: "net_dhcp", section: "network", title: "Obtain address automatically", type: "bool",
    default: 1, reboot: true },

  { key: "sec_https", section: "security", title: "Serve over HTTPS", type: "bool", default: 1,
    requires: "https", reboot: true },
  { key: "sec_user", section: "security", title: "Username", type: "string", maxLength: 31,
    default: "admin", requires: "https" },

  { key: "upd_check", section: "system", title: "Offer firmware updates", type: "bool",
    default: 0, requires: "ota",
    help: "The browser asks the address below whether a newer build exists and offers to install it. The device never reaches out on its own." },
  { key: "upd_url", section: "system", title: "Update manifest", type: "string",
    default: "", maxLength: 200, requires: "ota",
    help: "URL of a manifest.json describing the newest build." },
  { key: "log_level", section: "system", title: "Log verbosity", type: "enum",
    choices: ["error", "warn", "info", "debug"], default: 2 },
];

/* Matches what the real device reports today: video and input work, the rest
 * is either absent hardware or not built yet. */
const CAPS = {
  video: { compiled: true, available: true, enabled: true, active: true },
  mjpeg: { compiled: true, available: true, enabled: true, active: true },
  h264: { compiled: true, available: true, enabled: true, active: true },
  hid: { compiled: true, available: true, enabled: true, active: true },
  msc: { compiled: true, available: false, enabled: false, active: false, setting: "msc_enable",
         reason: "virtual media not implemented yet" },
  atx: { compiled: true, available: false, enabled: false, active: false, setting: "atx_enable",
         reason: "power control not implemented yet" },
  audio: { compiled: false, available: false, enabled: false, active: false,
           reason: "not built into this firmware" },
  https: { compiled: true, available: false, enabled: true, active: false,
           reason: "TLS and login not implemented yet" },
  ota: { compiled: true, available: false, enabled: true, active: false,
         reason: "partition table has no second app slot" },
};

const values = Object.fromEntries(
  SCHEMA.map((s) => [s.key, s.type === "string" ? s.default : s.default]),
);

/** Scenario knobs, driven from /mock/state so states can be forced on demand. */
const state = {
  signal: true,
  attached: true,
  leds: 0,
  width: 1920,
  height: 1080,
  modeChanges: 1,
  viewers: 1,
  /* Driven from /mock/state so the thermal states can be looked at without a
     hot chip. */
  tempC: 44.3,
  thermal: "normal",
};

function json(res, body, status = 200) {
  res.statusCode = status;
  res.setHeader("Content-Type", "application/json");
  res.end(JSON.stringify(body));
}

async function readBody(req) {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  return Buffer.concat(chunks).toString("utf8");
}

export function mockDevice() {
  return {
    name: "espkvm-mock-device",
    apply: "serve",
    configureServer(server) {
      const wss = new WebSocketServer({ noServer: true });

      server.httpServer?.on("upgrade", (req, socket, head) => {
        if (!req.url.startsWith("/ws")) return;
        wss.handleUpgrade(req, socket, head, (ws) => {
          const sendStatus = () =>
            ws.send(Buffer.from([0x81, state.attached ? 1 : 0, state.leds]));
          ws.on("message", (data) => {
            const b = Buffer.from(data);
            if (b[0] === 0x06) {
              ws.send(Buffer.from([0x82]));
              sendStatus();
            }
          });
          state.wsSend = sendStatus;
        });
      });

      server.middlewares.use((req, res, next) => {
        const url = req.url.split("?")[0];

        /*
         * The device answers "who is asking?" before anything else, so the
         * mock has to as well - otherwise the console sits on its sign-in
         * screen forever, or worse, fails to start. The mock is always logged
         * in: there is nothing here worth protecting.
         */
        if (url === "/api/v1/auth/session") {
          return json(res, { required: false, authenticated: true, mustChange: false, user: "admin" });
        }
        if (url === "/api/v1/auth/login" && req.method === "POST") {
          return json(res, { mustChange: false });
        }
        if (url === "/api/v1/auth/logout" && req.method === "POST") {
          return json(res, { status: "logged out" });
        }
        if (url === "/api/v1/auth/password" && req.method === "POST") {
          return json(res, { status: "changed" });
        }

        if (url === "/api/v1/system/info") {
          return json(res, {
            project: "espkvm",
            version: "mock",
            built: "mock build",
            idf: "v6.0.1",
            partition: "ota_0",
            updatable: true,
            uptimeSeconds: Math.floor(process.uptime()),
            heapFree: 14_000_000,
            psramFree: 13_500_000,
            tempC: state.tempC,
            thermal: state.thermal,
          });
        }
        if (url === "/api/v1/system/restart" && req.method === "POST") {
          return json(res, { status: "restarting" });
        }

        if (url === "/api/capabilities") return json(res, CAPS);
        if (url === "/api/v1/settings/schema") return json(res, SCHEMA);
        if (url === "/api/v1/settings" && req.method === "GET") return json(res, values);
        if (url === "/api/v1/settings" && req.method === "PUT") {
          return readBody(req).then((body) => {
            let patch;
            try {
              patch = JSON.parse(body);
            } catch {
              return json(res, { error: "body must be a JSON object" }, 400);
            }
            for (const [k, v] of Object.entries(patch)) {
              const entry = SCHEMA.find((s) => s.key === k);
              if (!entry) return json(res, { error: `unknown setting '${k}'` }, 404);
              if (entry.type === "int" && (v < entry.min || v > entry.max)) {
                return json(res, { error: `'${k}' must be between ${entry.min} and ${entry.max}` }, 400);
              }
              values[k] = typeof v === "boolean" ? (v ? 1 : 0) : v;
            }
            return json(res, values);
          });
        }
        if (url === "/api/v1/settings/reset" && req.method === "POST") {
          for (const s of SCHEMA) values[s.key] = s.default;
          return json(res, values);
        }
        if (url === "/api/v1/video/status") {
          const moving = Date.now() % 20000 < 10000;
          return json(res, {
            signal: state.signal,
            width: state.width,
            height: state.height,
            interlaced: false,
            fps: state.signal ? (moving ? 12.9 : 0) : 0,
            skippedFps: state.signal ? (moving ? 6.9 : 19) : 0,
            kbps: state.signal ? (moving ? 8507 : 0) : 0,
            modeChanges: state.modeChanges,
            sysStatus: state.signal ? 159 : 25,
            viewers: state.viewers,
            codec: "mjpeg",
          });
        }

        if (url === "/stream") {
          res.setHeader("Content-Type", "multipart/x-mixed-replace; boundary=frame");
          res.setHeader("Cache-Control", "no-store");
          let tick = 0;
          let alive = true;
          const push = () => {
            if (!alive) return;
            if (!state.signal) {
              /* No signal means no frames at all, exactly as the firmware
                 behaves - the UI has to notice on its own. */
              setTimeout(push, 500);
              return;
            }
            const png = renderFrame(state.width, state.height, tick++);
            res.write(`--frame\r\nContent-Type: image/png\r\nContent-Length: ${png.length}\r\n\r\n`);
            res.write(png);
            res.write("\r\n");
            setTimeout(push, 100);
          };
          req.on("close", () => {
            alive = false;
          });
          push();
          return;
        }

        /* Scenario control: POST /mock/state {"signal":false} and so on. */
        if (url === "/mock/state" && req.method === "POST") {
          return readBody(req).then((body) => {
            Object.assign(state, JSON.parse(body));
            state.wsSend?.();
            return json(res, state);
          });
        }
        if (url === "/mock/state") return json(res, state);

        return next();
      });
    },
  };
}
