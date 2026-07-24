<script setup lang="ts">
/*
 * Everything the operator's own browser will not pass through.
 *
 * Ctrl+Alt+Del, Alt+Tab and the Meta key are swallowed by the local OS long
 * before the page sees them, so a remote console has to offer them as buttons.
 * Pasting is the same problem in reverse: the clipboard holds characters, but
 * a KVM can only send key positions, so the text is translated through the
 * layout the target is known to have active.
 */
import { computed, ref } from "vue";

import { macrosForOs, macroLabel, SYSRQ_MOD, SYSRQ_KEY, SYSRQ_LETTERS } from "../macros.js";
import { charToHid, untypeableChars, DEFAULT_LAYOUT } from "../layouts.js";
import type { Control } from "../input/control";
import { enumName, type Setting, type Values } from "../state/device";
import { toast } from "../state/toasts";

const props = defineProps<{
  control: Control;
  schema: Setting[];
  values: Values;
  attached: boolean;
  /** OS guessed from USB enumeration; "auto" resolves to this. */
  detectedOs: string;
}>();

const layout = computed(() => enumName(props.schema, props.values, "kbd_layout") ?? DEFAULT_LAYOUT);
const typeDelay = computed(() => Math.max(1, Number(props.values.type_delay) || 8));
const pasting = ref(false);
const macroBusy = ref(false);

/* The OS whose conventions the panel follows: the manual setting, or the
   detected one when that setting is left on "auto". */
const effectiveOs = computed(() => {
  const choice = enumName(props.schema, props.values, "target_os") ?? "auto";
  return choice === "auto" ? props.detectedOs || "unknown" : choice;
});

interface Macro {
  id: string;
  label: string;
  labelByOs?: Record<string, string>;
  os?: string;
  modifier: number;
  key: number;
  sysrq?: string;
  destructive?: boolean;
  hint?: string;
}

const macros = computed(() => macrosForOs(effectiveOs.value) as Macro[]);
const labelFor = (m: Macro) => macroLabel(m, effectiveOs.value);

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/* Deliver a magic-SysRq sequence: hold Alt+SysRq, tap each command letter in
   turn, then let go. Sync (s) and remount-read-only (u) need a moment to finish
   before the next command, so they get a longer pause; a reboot/power-off (b/o)
   ends it. */
async function sendSysrq(letters: string) {
  macroBusy.value = true;
  try {
    props.control.keyboard(SYSRQ_MOD, [SYSRQ_KEY]);
    await sleep(400);
    for (const ch of letters.toLowerCase()) {
      const code = (SYSRQ_LETTERS as Record<string, number>)[ch];
      if (!code) continue;
      props.control.keyboard(SYSRQ_MOD, [SYSRQ_KEY, code]);
      await sleep(300);
      props.control.keyboard(SYSRQ_MOD, [SYSRQ_KEY]);
      await sleep(ch === "s" || ch === "u" ? 2500 : 800);
    }
  } finally {
    props.control.releaseAll();
    macroBusy.value = false;
  }
}

function sendMacro(m: Macro) {
  if (macroBusy.value) return;
  if (m.destructive) {
    const detail = m.hint ? `\n\n${m.hint}` : "";
    if (!confirm(`Send ${labelFor(m)} to the target?${detail}`)) return;
  }
  if (m.sysrq) {
    void sendSysrq(m.sysrq);
    return;
  }
  props.control.keyboard(m.modifier, m.key ? [m.key] : []);
  setTimeout(() => props.control.keyboard(0, []), 40);
}

/* The manual escape hatch: one message that clears keyboard, both mouse
   reports and the consumer control on the device at once. */
function releaseAll() {
  props.control.releaseAll();
}

async function paste() {
  if (!navigator.clipboard?.readText) {
    toast.error("Clipboard access needs HTTPS or localhost");
    return;
  }
  let text: string;
  try {
    text = await navigator.clipboard.readText();
  } catch {
    toast.error("Clipboard read was denied");
    return;
  }
  if (!text) return;

  /* Silently dropping characters would leave the operator believing the paste
     succeeded. Name the ones this layout cannot produce. */
  const missing = untypeableChars(layout.value, text) as string[];
  if (missing.length) {
    toast.error(`Layout ${layout.value} cannot type: ${missing.slice(0, 12).join(" ")}`);
  }

  pasting.value = true;
  try {
    for (const ch of text) {
      if (ch === "\r") continue;
      const row = charToHid(layout.value, ch) as { mod: number; hid: number } | null;
      if (!row) continue;
      props.control.keyboard(row.mod, [row.hid]);
      await sleep(typeDelay.value + 12);
      props.control.keyboard(0, []);
      await sleep(typeDelay.value);
    }
  } finally {
    pasting.value = false;
  }
}
</script>

<template>
  <div class="input-panel">
    <p v-if="!attached" class="section-blocked">
      No target on USB. Keystrokes will go nowhere until the cable is connected.
    </p>

    <h3>Key combinations</h3>
    <div class="macro-bar">
      <button
        v-for="m in macros"
        :key="m.id"
        type="button"
        :class="['btn', 'btn-sm', { 'btn-danger': m.destructive }]"
        :title="m.hint"
        :disabled="macroBusy"
        @click="sendMacro(m)"
      >
        {{ labelFor(m) }}
      </button>
    </div>

    <h3>Paste text</h3>
    <p class="setting-note">
      Sent as key positions using the <strong>{{ layout }}</strong> layout, which must match
      what the target has active. Change it under Settings -> Input.
    </p>
    <button type="button" class="btn btn-sm" :disabled="pasting" @click="paste">
      {{ pasting ? "Typing..." : "Paste from clipboard" }}
    </button>

    <h3>Stuck input</h3>
    <p class="setting-note">
      Lift every key and mouse button the target thinks is held. Use it if a
      click or a key ever gets stuck down - it is the software version of
      unplugging the cable.
    </p>
    <button type="button" class="btn btn-sm" @click="releaseAll">Release all inputs</button>
  </div>
</template>
