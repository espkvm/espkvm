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
import {
  runMacroScript,
  parseMacroScript,
  loadMacros,
  serializeMacros,
  type Macro as UserMacro,
} from "../macroScript";
import type { Control } from "../input/control";
import { enumName, saveSettings, type Setting, type Values } from "../state/device";
import { toast } from "../state/toasts";

const props = defineProps<{
  control: Control;
  schema: Setting[];
  values: Values;
  attached: boolean;
  /** OS guessed from USB enumeration; "auto" resolves to this. */
  detectedOs: string;
}>();

const emit = defineEmits<{ (e: "values", v: Values): void }>();

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

/* ---- user macros -------------------------------------------------------- */

const userMacros = computed<UserMacro[]>(() => loadMacros(props.values.macros_json));
const runningMacro = ref<string | null>(null);

/* The editor: index -1 means a new macro, >= 0 edits an existing one, null hides
   it. Parsing the draft on the fly gives the operator an error before running. */
const editIndex = ref<number | null>(null);
const editName = ref("");
const editScript = ref("");
const savingMacro = ref(false);

const scriptError = computed(() => {
  if (editIndex.value === null || !editScript.value.trim()) return null;
  try {
    parseMacroScript(editScript.value);
    return null;
  } catch (e) {
    return e instanceof Error ? e.message : String(e);
  }
});

function newMacro() {
  editIndex.value = -1;
  editName.value = "";
  editScript.value = "key ctrl+alt+delete\ndelay 500\ntype hello";
}

function editMacro(i: number) {
  const m = userMacros.value[i];
  editIndex.value = i;
  editName.value = m.name;
  editScript.value = m.script;
}

function cancelEdit() {
  editIndex.value = null;
}

async function persistMacros(list: UserMacro[]) {
  savingMacro.value = true;
  try {
    emit("values", await saveSettings({ macros_json: serializeMacros(list) }));
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    savingMacro.value = false;
  }
}

async function saveMacro() {
  const name = editName.value.trim();
  if (!name) {
    toast.error("Give the macro a name");
    return;
  }
  if (scriptError.value) {
    toast.error(scriptError.value);
    return;
  }
  const list = userMacros.value.slice();
  const entry = { name, script: editScript.value };
  if (editIndex.value !== null && editIndex.value >= 0) {
    list[editIndex.value] = entry;
  } else {
    list.push(entry);
  }
  await persistMacros(list);
  editIndex.value = null;
}

async function deleteMacro(i: number) {
  if (!confirm(`Delete the macro "${userMacros.value[i].name}"?`)) return;
  const list = userMacros.value.slice();
  list.splice(i, 1);
  await persistMacros(list);
}

async function runMacro(m: UserMacro) {
  if (runningMacro.value) return;
  runningMacro.value = m.name;
  try {
    await runMacroScript(m.script, props.control, layout.value, typeDelay.value);
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    runningMacro.value = null;
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

    <h3>Macros</h3>
    <ul v-if="userMacros.length" class="image-list">
      <li v-for="(m, i) in userMacros" :key="i" class="image-row">
        <span class="image-name mono">{{ m.name }}</span>
        <span class="macro-actions">
          <button type="button" class="btn btn-sm" :disabled="!!runningMacro" @click="runMacro(m)">
            {{ runningMacro === m.name ? "Running..." : "Run" }}
          </button>
          <button type="button" class="btn btn-sm btn-quiet" @click="editMacro(i)">Edit</button>
          <button type="button" class="btn btn-sm btn-quiet" @click="deleteMacro(i)">Delete</button>
        </span>
      </li>
    </ul>
    <p v-else class="setting-note">
      A macro is a short script - key chords, typed text and delays - that you replay with one
      click. Handy for a fixed sequence like stepping through a BIOS.
    </p>

    <div v-if="editIndex !== null" class="macro-editor">
      <input v-model="editName" type="text" placeholder="Macro name" />
      <textarea
        v-model="editScript"
        rows="6"
        class="mono"
        spellcheck="false"
        placeholder="key ctrl+alt+f2&#10;delay 500&#10;type root&#10;key enter"
      ></textarea>
      <p class="setting-note">
        One command per line: <code>key ctrl+alt+f2</code>, <code>type some text</code>,
        <code>delay 500</code>. Lines starting with # are ignored.
      </p>
      <p v-if="scriptError" class="setting-note setting-note-blocked">{{ scriptError }}</p>
      <div class="macro-actions">
        <button
          type="button"
          class="btn btn-sm"
          :disabled="savingMacro || !!scriptError"
          @click="saveMacro"
        >
          {{ savingMacro ? "Saving..." : "Save" }}
        </button>
        <button type="button" class="btn btn-sm btn-quiet" @click="cancelEdit">Cancel</button>
      </div>
    </div>
    <button v-else type="button" class="btn btn-sm" @click="newMacro">New macro...</button>

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
