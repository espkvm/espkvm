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
import { computed, ref, watch } from "vue";

import {
  SECTION_ORDER,
  SECTION_TITLES,
  type Capability,
  type Setting,
  type Values,
  type FirmwareRelease,
  compareVersions,
  downloadFirmware,
  deleteImage,
  fetchRelease,
  formatBytes,
  loadImages,
  resetSettings,
  saveSettings,
  settingBlockedReason,
  uploadFirmware,
  uploadImage,
  type StorageInfo,
  type SystemInfo,
} from "../state/device";
import { changePassword } from "../state/auth";
import { toast } from "../state/toasts";

const props = defineProps<{
  schema: Setting[];
  values: Values;
  caps: Record<string, Capability>;
  system: SystemInfo | null;
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

const uploading = ref(false);

async function onFirmwareChosen(e: Event) {
  const file = (e.target as HTMLInputElement).files?.[0];
  if (!file) return;
  if (!confirm(`Install ${file.name} and restart the device?`)) return;
  uploading.value = true;
  try {
    await uploadFirmware(file);
    toast.info("Firmware written, the device is restarting");
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    uploading.value = false;
    (e.target as HTMLInputElement).value = "";
  }
}

/*
 * The update check.
 *
 * The browser asks, not the device: on an isolated network the device has no
 * way out, and on any other it should not be taking one. What arrives is
 * handed to the same endpoint a manual upload uses, so there is one path into
 * the flash and one thing to trust.
 */
const release = ref<FirmwareRelease | null>(null);
const checking = ref(false);
const checkError = ref<string | null>(null);

const updateUrl = computed(() => String(props.values.upd_url ?? "").trim());
const updateEnabled = computed(() => Boolean(props.values.upd_check) && updateUrl.value !== "");
/*
 * What the published build is, relative to the running one. Version numbers are
 * compared as numbers - "v1.10.0" is newer than "v1.2.0", which a string
 * comparison gets backwards - and a device running an untagged build cannot be
 * ordered against a release at all, so it is offered the choice rather than
 * told it is behind.
 */
const updateState = computed<"none" | "newer" | "same" | "older" | "unknown">(() => {
  const published = release.value?.version;
  const running = props.system?.version;
  if (!published || !running) return "none";
  const order = compareVersions(published, running);
  if (order === null) return published === running ? "same" : "unknown";
  if (order > 0) return "newer";
  return order === 0 ? "same" : "older";
});
const updateAvailable = computed(
  () => updateState.value === "newer" || updateState.value === "unknown",
);

async function checkForUpdate() {
  if (!updateEnabled.value) return;
  checking.value = true;
  checkError.value = null;
  try {
    release.value = await fetchRelease(updateUrl.value);
  } catch (err) {
    release.value = null;
    checkError.value = err instanceof Error ? err.message : String(err);
  } finally {
    checking.value = false;
  }
}

/* Check when the panel first has what it needs, and again whenever the address
   or the switch changes - not on a timer: nobody needs a background poll to
   the internet from a page that is open for minutes at a time. */
watch(updateEnabled, (on) => (on ? void checkForUpdate() : (release.value = null)), {
  immediate: true,
});
watch(updateUrl, () => void checkForUpdate());

async function installRelease() {
  const target = release.value;
  if (!target) return;
  if (!confirm(`Install ${target.version} and restart the device?`)) return;
  uploading.value = true;
  try {
    const image = await downloadFirmware(target);
    await uploadFirmware(image);
    toast.info(`${target.version} written, the device is restarting`);
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    uploading.value = false;
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

/*
 * Virtual media.
 *
 * The card is the store; this panel lists what is on it, uploads more and
 * chooses which image the target sees. Choosing writes the msc_image setting,
 * the same value the text field above holds, so the two always agree.
 */
const storage = ref<StorageInfo | null>(null);
const loadingImages = ref(false);
const uploadingImage = ref(false);
const uploadPct = ref(0);

async function refreshImages() {
  loadingImages.value = true;
  try {
    storage.value = await loadImages();
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    loadingImages.value = false;
  }
}

/* Load the card's contents when the operator opens the media tab, not before:
   an idle console has no reason to be reading the directory. */
watch(
  currentSection,
  (s) => {
    if (s === "storage") void refreshImages();
  },
  { immediate: true },
);

async function onImageChosen(e: Event) {
  const input = e.target as HTMLInputElement;
  const file = input.files?.[0];
  if (!file) return;
  uploadingImage.value = true;
  uploadPct.value = 0;
  try {
    await uploadImage(file, (f) => (uploadPct.value = Math.round(f * 100)));
    toast.info(`${file.name} uploaded`);
    await refreshImages();
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    uploadingImage.value = false;
    input.value = "";
  }
}

async function selectImage(name: string) {
  try {
    emit("values", await saveSettings({ msc_image: name }));
    if (storage.value) storage.value.active = name;
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  }
}

async function removeImage(name: string) {
  if (!confirm(`Delete ${name} from the card?`)) return;
  try {
    storage.value = await deleteImage(name);
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
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

    <div v-if="currentSection === 'storage'" class="firmware">
      <h3>Images on the card</h3>
      <p v-if="loadingImages && !storage" class="setting-note">Reading the card...</p>
      <p v-else-if="storage && !storage.mounted" class="section-blocked">
        No microSD card is mounted. Insert one formatted as FAT32 with your boot
        images copied on (up to 4&nbsp;GB per file).
      </p>
      <template v-else-if="storage">
        <p class="setting-note">
          {{ formatBytes(storage.freeBytes) }} free of {{ formatBytes(storage.totalBytes) }}.
          The chosen image is served to the target read-only; turn on
          <em>Expose virtual media</em> above for it to appear.
        </p>
        <p v-if="!storage.writable" class="setting-note setting-note-blocked">
          {{ storage.writeReason ?? "The card is read-only on this device." }}
          Format it FAT32 and copy images in a card reader &mdash; one file up to
          4&nbsp;GB (a FAT32 limit) &mdash; then pick one below.
        </p>

        <ul class="image-list">
          <li
            v-for="img in storage.images"
            :key="img.name"
            :class="['image-row', { 'image-active': img.name === storage.active }]"
          >
            <label class="image-pick">
              <input
                type="radio"
                name="active-image"
                :checked="img.name === storage.active"
                @change="selectImage(img.name)"
              />
              <span class="mono image-name">{{ img.name }}</span>
              <span class="muted">{{ formatBytes(img.size) }}</span>
            </label>
            <button
              v-if="storage.writable"
              type="button"
              class="btn btn-sm btn-quiet"
              @click="removeImage(img.name)"
            >
              Delete
            </button>
          </li>
          <li v-if="storage.images.length === 0" class="muted image-empty">
            No images on the card yet. Upload one below.
          </li>
        </ul>

        <label class="image-pick image-eject">
          <input
            type="radio"
            name="active-image"
            :checked="!storage.active"
            @change="selectImage('')"
          />
          <span>Eject &mdash; offer the target no medium</span>
        </label>

        <label
          v-if="storage.writable"
          :class="['btn', 'btn-sm', { 'btn-disabled': uploadingImage }]"
        >
          {{ uploadingImage ? `Uploading ${uploadPct}%...` : "Upload image..." }}
          <input type="file" class="sr-only" :disabled="uploadingImage" @change="onImageChosen" />
        </label>
      </template>
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

    <div v-if="currentSection === 'system' && system" class="firmware">
      <h3>Firmware</h3>
      <dl class="facts">
        <div class="fact"><dt>Version</dt><dd class="mono">{{ system.version }}</dd></div>
        <div class="fact"><dt>Built</dt><dd class="mono">{{ system.built }}</dd></div>
        <div class="fact"><dt>Running from</dt><dd class="mono">{{ system.partition }}</dd></div>
        <div v-if="system.tempC > 0" class="fact">
          <dt>Chip temperature</dt>
          <dd class="mono">{{ system.tempC.toFixed(1) }} C</dd>
        </div>
        <div class="fact">
          <dt>Free memory</dt>
          <dd class="mono">
            {{ Math.round(system.heapFree / 1024) }}K heap,
            {{ Math.round(system.psramFree / 1024 / 1024) }}M PSRAM
          </dd>
        </div>
        <div class="fact">
          <dt>Uptime</dt>
          <dd class="mono">{{ Math.floor(system.uptimeSeconds / 60) }} min</dd>
        </div>
      </dl>
      <p v-if="!system.updatable" class="section-blocked">
        This firmware has a single app slot, so it cannot update itself.
      </p>
      <template v-else>
        <p class="setting-note">
          The image is written to the spare slot. If it fails to start, the device returns to
          this one on its own.
        </p>

        <div v-if="updateEnabled" class="update">
          <p v-if="checking" class="setting-note">Checking for a newer build...</p>
          <p v-else-if="checkError" class="setting-note setting-note-blocked">
            Could not read the update manifest: {{ checkError }}
          </p>
          <template v-else-if="release">
            <p v-if="updateState === 'newer'" class="setting-note">
              <strong>{{ release.version }}</strong> is published; this device runs
              {{ system.version }}.
              <a v-if="release.notes" :href="release.notes" target="_blank" rel="noreferrer">
                What changed
              </a>
            </p>
            <p v-else-if="updateState === 'unknown'" class="setting-note">
              <strong>{{ release.version }}</strong> is published. This device runs
              {{ system.version }}, which is not a release, so which is newer is anyone's guess.
            </p>
            <p v-else-if="updateState === 'older'" class="setting-note">
              This device runs {{ system.version }}, ahead of the published
              {{ release.version }}.
            </p>
            <p v-else class="setting-note">This device runs the newest published build.</p>
            <button
              v-if="updateAvailable"
              type="button"
              class="btn btn-sm"
              :disabled="uploading"
              @click="installRelease"
            >
              {{ uploading ? "Installing..." : `Install ${release.version}` }}
            </button>
          </template>
          <button
            type="button"
            class="btn btn-sm btn-quiet"
            :disabled="checking"
            @click="checkForUpdate"
          >
            Check again
          </button>
        </div>
        <label :class="['btn', 'btn-sm', { 'btn-disabled': uploading }]">
          {{ uploading ? "Uploading..." : "Install firmware..." }}
          <input
            type="file"
            accept=".bin"
            class="sr-only"
            :disabled="uploading"
            @change="onFirmwareChosen"
          />
        </label>
      </template>
    </div>

    <div class="settings-footer">
      <button type="button" class="btn btn-sm btn-danger" :disabled="busy" @click="doReset">
        Restore defaults
      </button>
    </div>
  </div>
</template>
