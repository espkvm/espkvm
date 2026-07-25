<script setup lang="ts">
/*
 * The console shell: a status strip that never lies about what the device is
 * doing, a rail of panels that slide over the picture rather than displacing
 * it, and the target's screen filling everything else.
 */
import { computed, onMounted, onUnmounted, ref } from "vue";

import Icon from "./components/Icon.vue";
import InputPanel from "./components/InputPanel.vue";
import LoginView from "./components/LoginView.vue";
import ScreenView from "./components/ScreenView.vue";
import DiagWidget from "./components/DiagWidget.vue";
import OsWidget from "./components/OsWidget.vue";
import MediaPanel from "./components/MediaPanel.vue";
import SettingsPanel from "./components/SettingsPanel.vue";
import ToastHost from "./components/ToastHost.vue";
import UpdateWidget from "./components/UpdateWidget.vue";
import { useInput } from "./input/useInput";
import {
  type Capability,
  type Setting,
  type Values,
  type SystemInfo,
  type VideoStatus,
  enumName,
  loadCapabilities,
  loadSchema,
  loadSystemInfo,
  loadUsbProbe,
  loadValues,
  loadVideoStatus,
  wakeTarget,
  type UsbProbe,
  Unauthorized,
} from "./state/device";
import { loadSession, type SessionState } from "./state/auth";
import { toast } from "./state/toasts";

type PanelId = "video" | "input" | "media" | "power" | "settings" | null;

const PANEL_TITLES: Record<string, string> = {
  video: "Video",
  input: "Input",
  media: "Virtual media",
  power: "Power",
  settings: "Settings",
};

const schema = ref<Setting[]>([]);
const values = ref<Values>({});
const caps = ref<Record<string, Capability>>({});
const status = ref<VideoStatus | null>(null);
const system = ref<SystemInfo | null>(null);
const usbProbe = ref<UsbProbe | null>(null);

/* Wake-on-LAN: the target MAC is a setting, the button is here. */
const wolMac = computed(() => String(values.value.pwr_wol_mac ?? "").trim());
const waking = ref(false);
async function wake() {
  waking.value = true;
  try {
    await wakeTarget();
    toast.info("Wake-on-LAN packet sent");
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    waking.value = false;
  }
}
const ready = ref(false);
const loadError = ref<string | null>(null);
const session = ref<SessionState | null>(null);
/* Nothing is loaded until the device says who is asking. */
const locked = computed(
  () => session.value !== null && session.value.required && !session.value.authenticated,
);
const mustChange = computed(() => Boolean(session.value?.mustChange));

const panel = ref<PanelId>(null);
const fit = ref<"fit" | "actual">("fit");
const engaged = ref(false);
const paused = ref(false);
const theme = ref<"dark" | "light">("dark");
const surface = ref<HTMLElement | null>(null);

const engageMode = computed(
  () => (enumName(schema.value, values.value, "ptr_engage") as "click" | "hover") ?? "click",
);
const pointerMode = computed(
  () =>
    (enumName(schema.value, values.value, "mouse_mode") as
      | "absolute"
      | "relative"
      | "auto") ?? "absolute",
);
const invertScroll = computed(() => Boolean(values.value.scroll_inv));
/* Which side the rail and its panels sit on, from the ui_side setting. */
const uiRight = computed(() => enumName(schema.value, values.value, "ui_side") === "right");

const input = useInput({
  engaged,
  engageMode,
  pointerMode,
  invertScroll,
  surface,
  onDisengage: () => (engaged.value = false),
});

/* What is actually being encoded, not what was asked for: an encoder that
   fails to start falls back, and the status bar should show the truth. */
const codec = computed(
  () => status.value?.codec || enumName(schema.value, values.value, "vid_codec") || "-",
);
const online = computed(() => status.value !== null);

let pollId = 0;
let systemPollId = 0;

async function startConsole() {
  try {
    const [s, v, c, sys] = await Promise.all([
      loadSchema(),
      loadValues(),
      loadCapabilities(),
      loadSystemInfo(),
    ]);
    schema.value = s;
    values.value = v;
    caps.value = c;
    system.value = sys;
    ready.value = true;
  } catch (err) {
    loadError.value = err instanceof Error ? err.message : String(err);
  }

  /* Telemetry is polled rather than pushed: one small request a second, and it
     keeps working when the control socket is down - exactly when the operator
     most wants to know what the device thinks. */
  let reachable = true;
  /* Backs off while the device is unreachable. A console that keeps asking
     once a second is not free: several tabs left open on a device that is
     rebooting, or refusing a certificate, turn into a load that makes it
     harder to reach - which is exactly the wrong direction. */
  let interval = 1000;
  const tick = async () => {
    try {
      const s = await loadVideoStatus();
      if (!reachable) {
        reachable = true;
        toast.info("Device is back");
      }
      interval = 1000;
      status.value = s;
    } catch (err) {
      if (err instanceof Unauthorized) {
        /* Not a network problem: the device is answering, it just does not
           know us any more. Stop polling and let the sign-in form take over -
           reporting "lost contact" here would send the operator looking for a
           fault that is not there. */
        clearInterval(systemPollId);
        status.value = null;
        session.value = await loadSession().catch(() => session.value);
        return;
      }
      status.value = null;
      interval = Math.min(interval * 2, 15000);
      /* Say it once. A console that silently freezes on the last frame is how
         an operator ends up typing into a machine that is not listening. */
      if (reachable) {
        reachable = false;
        toast.error("Lost contact with the device");
      }
    }
    pollId = window.setTimeout(tick, interval);
  };
  void tick();

  /* System figures change slowly - temperature, uptime, free memory - but they
     do change, and a page left open for an hour showing the values it loaded
     with is worse than showing none. */
  clearInterval(systemPollId);
  systemPollId = window.setInterval(async () => {
    try {
      system.value = await loadSystemInfo();
      usbProbe.value = await loadUsbProbe();
    } catch {
      /* The video poll reports loss of contact, and handles being signed out. */
    }
  }, 10000);

  usbProbe.value = await loadUsbProbe().catch(() => null);

  window.addEventListener("keydown", onGlobalKey);
}

onMounted(async () => {
  document.documentElement.dataset.theme = theme.value;
  try {
    session.value = await loadSession();
  } catch (err) {
    loadError.value = err instanceof Error ? err.message : String(err);
    return;
  }
  if (!locked.value && !mustChange.value) {
    await startConsole();
  }
});

/* Signing in, or changing the password, both end with asking the device again
   rather than assuming what happened. */
async function onAuthenticated() {
  session.value = await loadSession();
  if (!locked.value && !mustChange.value) await startConsole();
}

async function onPasswordChanged() {
  toast.info("Password changed - sign in again");
  panel.value = null;
  session.value = await loadSession();
}

onUnmounted(() => {
  clearTimeout(pollId);
  clearInterval(systemPollId);
  window.removeEventListener("keydown", onGlobalKey);
});

function onGlobalKey(e: KeyboardEvent) {
  if (engaged.value) return;
  if (e.key === "f" || e.key === "F") void toggleFullscreen();
}

function reload() {
  location.reload();
}

function toggleTheme() {
  theme.value = theme.value === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = theme.value;
}

function togglePanel(id: Exclude<PanelId, null>) {
  panel.value = panel.value === id ? null : id;
}

function onEngage(e: PointerEvent) {
  engaged.value = true;
  input.engageFromPointer(e);
}

function formatRate(kbps: number): string {
  if (kbps <= 0) return "idle";
  if (kbps < 1000) return `${kbps} kbit/s`;
  return `${(kbps / 1000).toFixed(1)} Mbit/s`;
}

async function toggleFullscreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.documentElement.requestFullscreen();
  } catch {
    /* denied by the browser; nothing useful to say */
  }
}

const LED_BITS: Array<[number, string]> = [
  [0x01, "Num"],
  [0x02, "Caps"],
  [0x04, "Scroll"],
];
</script>

<template>
  <div v-if="loadError" class="fatal">
    <h2>Cannot reach the device</h2>
    <p class="muted">{{ loadError }}</p>
    <button type="button" class="btn btn-primary" @click="reload">Retry</button>
  </div>

  <LoginView
    v-else-if="locked || mustChange"
    :user="session?.user ?? 'admin'"
    :must-change="mustChange"
    @authenticated="onAuthenticated"
    @changed="onPasswordChanged"
  />

  <div v-else class="console">
    <header class="statusbar">
      <svg
        class="brand"
        viewBox="6 15 51 33"
        width="34"
        height="22"
        role="img"
        aria-label="ESP-KVM"
      >
        <title>ESP-KVM</title>
        <g fill="currentColor">
          <rect x="6" y="15" width="3" height="3"/>
          <rect x="9" y="15" width="3" height="3"/>
          <rect x="12" y="15" width="3" height="3"/>
          <rect x="15" y="15" width="3" height="3"/>
          <rect x="18" y="15" width="3" height="3"/>
          <rect x="6" y="18" width="3" height="3"/>
          <rect x="6" y="21" width="3" height="3"/>
          <rect x="9" y="21" width="3" height="3"/>
          <rect x="12" y="21" width="3" height="3"/>
          <rect x="15" y="21" width="3" height="3"/>
          <rect x="6" y="24" width="3" height="3"/>
          <rect x="6" y="27" width="3" height="3"/>
          <rect x="9" y="27" width="3" height="3"/>
          <rect x="12" y="27" width="3" height="3"/>
          <rect x="15" y="27" width="3" height="3"/>
          <rect x="18" y="27" width="3" height="3"/>
          <rect x="24" y="15" width="3" height="3"/>
          <rect x="27" y="15" width="3" height="3"/>
          <rect x="30" y="15" width="3" height="3"/>
          <rect x="33" y="15" width="3" height="3"/>
          <rect x="36" y="15" width="3" height="3"/>
          <rect x="24" y="18" width="3" height="3"/>
          <rect x="24" y="21" width="3" height="3"/>
          <rect x="27" y="21" width="3" height="3"/>
          <rect x="30" y="21" width="3" height="3"/>
          <rect x="33" y="21" width="3" height="3"/>
          <rect x="36" y="21" width="3" height="3"/>
          <rect x="36" y="24" width="3" height="3"/>
          <rect x="24" y="27" width="3" height="3"/>
          <rect x="27" y="27" width="3" height="3"/>
          <rect x="30" y="27" width="3" height="3"/>
          <rect x="33" y="27" width="3" height="3"/>
          <rect x="36" y="27" width="3" height="3"/>
          <rect x="42" y="15" width="3" height="3"/>
          <rect x="45" y="15" width="3" height="3"/>
          <rect x="48" y="15" width="3" height="3"/>
          <rect x="51" y="15" width="3" height="3"/>
          <rect x="54" y="15" width="3" height="3"/>
          <rect x="42" y="18" width="3" height="3"/>
          <rect x="54" y="18" width="3" height="3"/>
          <rect x="42" y="21" width="3" height="3"/>
          <rect x="45" y="21" width="3" height="3"/>
          <rect x="48" y="21" width="3" height="3"/>
          <rect x="51" y="21" width="3" height="3"/>
          <rect x="54" y="21" width="3" height="3"/>
          <rect x="42" y="24" width="3" height="3"/>
          <rect x="42" y="27" width="3" height="3"/>
          <rect x="6" y="33" width="3" height="3"/>
          <rect x="18" y="33" width="3" height="3"/>
          <rect x="6" y="36" width="3" height="3"/>
          <rect x="15" y="36" width="3" height="3"/>
          <rect x="6" y="39" width="3" height="3"/>
          <rect x="9" y="39" width="3" height="3"/>
          <rect x="12" y="39" width="3" height="3"/>
          <rect x="6" y="42" width="3" height="3"/>
          <rect x="15" y="42" width="3" height="3"/>
          <rect x="6" y="45" width="3" height="3"/>
          <rect x="18" y="45" width="3" height="3"/>
          <rect x="24" y="33" width="3" height="3"/>
          <rect x="36" y="33" width="3" height="3"/>
          <rect x="24" y="36" width="3" height="3"/>
          <rect x="36" y="36" width="3" height="3"/>
          <rect x="24" y="39" width="3" height="3"/>
          <rect x="36" y="39" width="3" height="3"/>
          <rect x="27" y="42" width="3" height="3"/>
          <rect x="33" y="42" width="3" height="3"/>
          <rect x="30" y="45" width="3" height="3"/>
          <rect x="42" y="33" width="3" height="3"/>
          <rect x="54" y="33" width="3" height="3"/>
          <rect x="42" y="36" width="3" height="3"/>
          <rect x="45" y="36" width="3" height="3"/>
          <rect x="51" y="36" width="3" height="3"/>
          <rect x="54" y="36" width="3" height="3"/>
          <rect x="42" y="39" width="3" height="3"/>
          <rect x="48" y="39" width="3" height="3"/>
          <rect x="54" y="39" width="3" height="3"/>
          <rect x="42" y="42" width="3" height="3"/>
          <rect x="54" y="42" width="3" height="3"/>
          <rect x="42" y="45" width="3" height="3"/>
          <rect x="54" y="45" width="3" height="3"/>
        </g>
      </svg>

      <span class="stat">
        <span
          :class="['dot', online ? (status!.signal ? 'dot-ok' : 'dot-warn') : 'dot-bad']"
        />
        {{ online ? (status!.signal ? "Online" : "No signal") : "Unreachable" }}
      </span>

      <template v-if="online && status!.signal">
        <span class="stat mono">{{ status!.width }}x{{ status!.height }}</span>
        <span class="stat mono" title="Codec and published frame rate">
          {{ codec }} {{ status!.fps.toFixed(1) }} fps
        </span>
        <span class="stat mono" title="Encoded frames skipped as unchanged">
          {{ status!.skippedFps.toFixed(0) }} skipped
        </span>
        <span class="stat mono" title="Outgoing video bitrate">
          {{ formatRate(status!.kbps) }}
        </span>
        <span
          v-if="status!.encoderBusyPct >= 90"
          class="stat mono warn"
          title="The encoder has no headroom left; lower the frame rate limit or the resolution"
        >
          encoder {{ status!.encoderBusyPct }}%
        </span>
      </template>

      <span class="statusbar-spacer" />

      <UpdateWidget :system="system" :values="values" />

      <button
        type="button"
        class="btn btn-sm btn-icon"
        :aria-label="theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme'"
        @click="toggleTheme"
      >
        <Icon :name="theme === 'dark' ? 'sun' : 'moon'" :size="15" />
      </button>
    </header>

    <div :class="['body', { 'layout-right': uiRight }]">
      <nav class="rail" aria-label="Panels">
        <button
          type="button"
          :class="['rail-btn', { 'rail-btn-active': panel === 'video' }]"
          aria-label="Video"
          @click="togglePanel('video')"
        >
          <Icon name="screen" :size="18" />
        </button>
        <button
          type="button"
          :class="['rail-btn', { 'rail-btn-active': panel === 'input' }]"
          aria-label="Input"
          @click="togglePanel('input')"
        >
          <Icon name="keyboard" :size="18" />
        </button>
        <button
          type="button"
          :class="['rail-btn', { 'rail-btn-active': panel === 'media' }]"
          aria-label="Virtual media"
          :disabled="!caps.msc?.available"
          :title="caps.msc?.reason ?? 'Virtual media'"
          @click="togglePanel('media')"
        >
          <Icon name="disc" :size="18" />
        </button>
        <button
          type="button"
          :class="['rail-btn', { 'rail-btn-active': panel === 'power' }]"
          aria-label="Power"
          :disabled="!(caps.wol?.available || caps.atx?.available)"
          :title="caps.wol?.available || caps.atx?.available ? 'Power' : (caps.atx?.reason ?? 'Power')"
          @click="togglePanel('power')"
        >
          <Icon name="power" :size="18" />
        </button>
        <div class="rail-spacer" />
        <OsWidget
          :probe="usbProbe"
          :attached="input.target.value.attached"
          :side="uiRight ? 'right' : 'left'"
        />
        <DiagWidget :system="system" :side="uiRight ? 'right' : 'left'" />
        <button
          type="button"
          :class="['rail-btn', { 'rail-btn-active': panel === 'settings' }]"
          aria-label="Settings"
          @click="togglePanel('settings')"
        >
          <Icon name="settings" :size="18" />
        </button>
      </nav>

      <main class="stage">
        <ScreenView
          :status="status"
          :engaged="engaged"
          :engage-mode="engageMode"
          :fit="fit"
          :paused="paused"
          @surface="surface = $event"
        />

        <button
          v-if="!engaged && !paused"
          type="button"
          class="screen-engage"
          @pointerdown="onEngage($event)"
        >
          {{
            engageMode === "hover"
              ? "Pointer follows the mouse. Click to send keystrokes."
              : "Click to control the target"
          }}
          <span class="screen-engage-hint">Esc gives control back</span>
        </button>

        <aside v-if="panel" class="panel" :aria-label="PANEL_TITLES[panel]">
          <header class="panel-head">
            <h2>{{ PANEL_TITLES[panel] }}</h2>
            <button type="button" class="btn btn-sm" @click="panel = null">Close</button>
          </header>
          <div class="panel-body">
            <SettingsPanel
              v-if="panel === 'settings' && ready"
              :schema="schema"
              :values="values"
              :caps="caps"
              @values="values = $event"
              @password-changed="onPasswordChanged"
            />

            <dl v-else-if="panel === 'video' && status" class="facts">
              <div class="fact">
                <dt>Signal</dt>
                <dd class="mono">{{ status.signal ? "locked" : "absent" }}</dd>
              </div>
              <div class="fact">
                <dt>Mode</dt>
                <dd class="mono">
                  {{ status.width }}x{{ status.height }}{{ status.interlaced ? "i" : "p" }}
                </dd>
              </div>
              <div class="fact">
                <dt>Published</dt>
                <dd class="mono">{{ status.fps.toFixed(2) }} fps</dd>
              </div>
              <div class="fact">
                <dt>Skipped as unchanged</dt>
                <dd class="mono">{{ status.skippedFps.toFixed(2) }} fps</dd>
              </div>
              <div class="fact">
                <dt>Bitrate</dt>
                <dd class="mono">{{ formatRate(status.kbps) }}</dd>
              </div>
              <div class="fact">
                <dt title="Share of wall clock the encoder was busy">Encoder load</dt>
                <dd class="mono" :class="{ warn: status.encoderBusyPct >= 90 }">
                  {{ status.encoderBusyPct }}%
                </dd>
              </div>
              <div class="fact">
                <dt title="Mean time one frame took to encode">Encode time</dt>
                <dd class="mono">{{ (status.encodeUs / 1000).toFixed(1) }} ms</dd>
              </div>
              <div class="fact">
                <dt>Mode changes</dt>
                <dd class="mono">{{ status.modeChanges }}</dd>
              </div>
              <div class="fact">
                <dt>Viewers</dt>
                <dd class="mono">{{ status.viewers }}</dd>
              </div>
              <div class="fact">
                <dt>Bridge SYS_STATUS</dt>
                <dd class="mono">0x{{ status.sysStatus.toString(16) }}</dd>
              </div>
            </dl>

            <InputPanel
              v-else-if="panel === 'input'"
              :control="input.control"
              :schema="schema"
              :values="values"
              :attached="input.target.value.attached"
              :detected-os="usbProbe?.os ?? 'unknown'"
              @values="values = $event"
            />
            <MediaPanel v-else-if="panel === 'media'" @values="values = $event" />
            <div v-else-if="panel === 'power'" class="power-panel">
              <template v-if="caps.wol?.available">
                <h3>Wake on LAN</h3>
                <p v-if="!wolMac" class="muted">
                  Set the target's MAC address under Settings &rarr; Power, then wake it from here.
                </p>
                <template v-else>
                  <p class="muted">
                    Send a magic packet to <span class="mono">{{ wolMac }}</span>. The target must
                    have Wake-on-LAN enabled and keep standby power.
                  </p>
                  <button type="button" class="btn btn-sm" :disabled="waking" @click="wake">
                    {{ waking ? "Sending..." : "Wake target" }}
                  </button>
                </template>
              </template>
              <p v-else class="muted">
                {{ caps.atx?.reason ?? "Power control is not available." }}
              </p>
            </div>
          </div>
        </aside>
      </main>
    </div>

    <footer class="actionbar">
      <div class="actionbar-left">
        <span class="stat">
          <span
            :class="[
              'dot',
              !input.target.value.known
                ? 'dot-bad'
                : input.target.value.attached
                  ? 'dot-ok'
                  : 'dot-warn',
            ]"
          />
          {{
            !input.target.value.known
              ? "Control channel down"
              : input.target.value.attached
                ? "USB attached"
                : "No target on USB"
          }}
        </span>

        <span class="leds">
          <span
            v-for="[bit, name] in LED_BITS"
            :key="name"
            :class="['led', { 'led-on': input.target.value.leds & bit }]"
          >
            {{ name }}
          </span>
        </span>
        <span class="muted">
          {{ engaged ? "Controlling - Esc releases" : "Not controlling" }}
        </span>
      </div>
      <div class="actionbar-right">
        <button
          type="button"
          class="btn btn-sm"
          :title="paused ? 'Resume the video stream' : 'Disconnect the stream to save bandwidth'"
          @click="paused = !paused"
        >
          {{ paused ? "Resume" : "Pause" }}
        </button>
        <button
          type="button"
          class="btn btn-sm"
          @click="fit = fit === 'fit' ? 'actual' : 'fit'"
        >
          {{ fit === "fit" ? "Fit" : "1:1" }}
        </button>
        <button
          type="button"
          class="btn btn-sm btn-icon"
          aria-label="Fullscreen"
          @click="toggleFullscreen()"
        >
          <Icon name="fullscreen" :size="15" />
        </button>
      </div>
    </footer>

    <ToastHost />
  </div>
</template>
