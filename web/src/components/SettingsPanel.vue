<script setup lang="ts">
/*
 * The settings panel renders itself from the schema the device serves, so a
 * setting added to the firmware table appears here with its title, range and
 * help text without any change in this file.
 *
 * A control whose capability the hardware lacks is shown disabled carrying the
 * device's own reason, rather than hidden. Hiding it would leave the operator
 * wondering whether the feature exists at all.
 */
import { computed, ref } from "vue";

import {
  SECTION_ORDER,
  SECTION_TITLES,
  type Capability,
  type Setting,
  type Values,
  resetSettings,
  saveSettings,
  settingBlockedReason,
} from "../state/device";
import { changePassword } from "../state/auth";
import { toast } from "../state/toasts";

const props = defineProps<{
  schema: Setting[];
  values: Values;
  caps: Record<string, Capability>;
}>();

const emit = defineEmits<{ values: [Values]; passwordChanged: [] }>();

const sections = computed(() => {
  const present = new Set(props.schema.map((s) => s.section));
  return SECTION_ORDER.filter((s) => present.has(s));
});

const active = ref("");
const currentSection = computed(() => active.value || sections.value[0] || "video");
const busy = ref(false);

const rows = computed(() => props.schema.filter((s) => s.section === currentSection.value));

/* When one missing capability blocks the whole section, say so once at the top
   rather than repeating the same sentence under every control. */
const sectionBlocked = computed(() => {
  const blockers = rows.value.map((r) => settingBlockedReason(r, props.caps));
  return rows.value.length > 0 && blockers.every((b) => b !== null && b === blockers[0])
    ? blockers[0]
    : null;
});

function blockedFor(s: Setting): string | null {
  return sectionBlocked.value ? null : settingBlockedReason(s, props.caps);
}

async function write(key: string, value: number | string | boolean) {
  busy.value = true;
  try {
    emit("values", await saveSettings({ [key]: value }));
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    busy.value = false;
  }
}


/*
 * Changing the password.
 *
 * It is not a setting and deliberately so: the settings API reads and writes
 * plain values, and a password that can be read back is not a password. It
 * goes to its own endpoint, which demands the current one and stores only a
 * salted hash.
 */
const currentPassword = ref("");
const newPassword = ref("");
const repeatPassword = ref("");
const changingPassword = ref(false);

const passwordTooShort = computed(
  () => newPassword.value.length > 0 && newPassword.value.length < 8,
);
const passwordMismatch = computed(
  () => repeatPassword.value.length > 0 && newPassword.value !== repeatPassword.value,
);

async function submitPassword() {
  if (passwordTooShort.value || passwordMismatch.value || !newPassword.value) return;
  changingPassword.value = true;
  try {
    await changePassword(currentPassword.value, newPassword.value);
    currentPassword.value = "";
    newPassword.value = "";
    repeatPassword.value = "";
    /* Every session ended, including this one - the console has to send the
     * operator back to the sign-in form rather than pretend otherwise. */
    emit("passwordChanged");
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    changingPassword.value = false;
  }
}

async function doReset() {
  if (!confirm("Restore every setting to its default?")) return;
  busy.value = true;
  try {
    emit("values", await resetSettings());
    toast.info("Settings restored to defaults");
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="settings">
    <div class="tabs" role="tablist">
      <button
        v-for="s in sections"
        :key="s"
        type="button"
        role="tab"
        :aria-selected="s === currentSection"
        :class="['tab', { 'tab-active': s === currentSection }]"
        @click="active = s"
      >
        {{ SECTION_TITLES[s] ?? s }}
      </button>
    </div>

    <p v-if="sectionBlocked" class="section-blocked">{{ sectionBlocked }}</p>

    <div class="settings-list">
      <div
        v-for="s in rows"
        :key="s.key"
        :class="['setting', { 'setting-blocked': busy || sectionBlocked || blockedFor(s) }]"
      >
        <div class="setting-head">
          <label class="setting-title" :for="`set-${s.key}`">{{ s.title }}</label>
          <span v-if="s.reboot" class="badge" title="Applies after a restart">restart</span>
        </div>

        <div class="setting-control">
          <label v-if="s.type === 'bool'" class="switch">
            <input
              :id="`set-${s.key}`"
              type="checkbox"
              :checked="Boolean(values[s.key])"
              :disabled="busy || !!sectionBlocked || !!blockedFor(s)"
              @change="write(s.key, ($event.target as HTMLInputElement).checked)"
            />
            <span class="muted">{{ values[s.key] ? "On" : "Off" }}</span>
          </label>

          <select
            v-else-if="s.type === 'enum'"
            :id="`set-${s.key}`"
            :disabled="busy || !!sectionBlocked || !!blockedFor(s)"
            :value="String(Number(values[s.key] ?? 0))"
            @change="write(s.key, Number(($event.target as HTMLSelectElement).value))"
          >
            <option v-for="(c, i) in s.choices ?? []" :key="c" :value="String(i)">{{ c }}</option>
          </select>

          <div v-else-if="s.type === 'int'" class="range-row">
            <input
              :id="`set-${s.key}`"
              type="range"
              :min="s.min"
              :max="s.max"
              :value="Number(values[s.key] ?? 0)"
              :disabled="busy || !!sectionBlocked || !!blockedFor(s)"
              @change="write(s.key, Number(($event.target as HTMLInputElement).value))"
            />
            <span class="mono range-value">{{ values[s.key] }}</span>
          </div>

          <input
            v-else
            :id="`set-${s.key}`"
            type="text"
            :maxlength="s.maxLength"
            :value="String(values[s.key] ?? '')"
            :disabled="busy || !!sectionBlocked || !!blockedFor(s)"
            @change="write(s.key, ($event.target as HTMLInputElement).value)"
          />
        </div>

        <p v-if="blockedFor(s)" class="setting-note setting-note-blocked">{{ blockedFor(s) }}</p>
        <p v-else-if="s.help" class="setting-note">{{ s.help }}</p>
      </div>
    </div>

    <form
      v-if="currentSection === 'security'"
      class="firmware"
      @submit.prevent="submitPassword"
    >
      <h3>Password</h3>
      <p class="setting-note">
        Not listed above with the other settings: those can be read back, and a password that
        can be read back is not one. Changing it signs out every open console.
      </p>
      <label class="field">
        <span>Current password</span>
        <input v-model="currentPassword" type="password" autocomplete="current-password" />
      </label>
      <label class="field">
        <span>New password</span>
        <input v-model="newPassword" type="password" autocomplete="new-password" />
      </label>
      <label class="field">
        <span>Repeat it</span>
        <input v-model="repeatPassword" type="password" autocomplete="new-password" />
      </label>
      <p v-if="passwordTooShort" class="setting-note">At least 8 characters.</p>
      <p v-else-if="passwordMismatch" class="setting-note">The two do not match.</p>
      <button
        type="submit"
        class="btn btn-sm"
        :disabled="changingPassword || passwordTooShort || passwordMismatch || !newPassword"
      >
        {{ changingPassword ? "Changing..." : "Change password" }}
      </button>
    </form>

    <div class="settings-footer">
      <button type="button" class="btn btn-sm btn-danger" :disabled="busy" @click="doReset">
        Restore defaults
      </button>
    </div>
  </div>
</template>
