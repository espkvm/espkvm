<script setup lang="ts">
/*
 * The version, as a small widget in the status bar: an outlined badge showing
 * what is running, a dot when a newer build is published, and - while an update
 * writes - the badge outline itself fills as a progress ring. Clicking it opens
 * the firmware panel (facts, the update check, install), so the whole update
 * flow lives one click from the version rather than buried in settings.
 *
 * The check runs in the browser, never on the device: a KVM sits on networks
 * with no way out, and one that quietly reaches the internet is not what belongs
 * there. The browser fetches the image and hands it to the same endpoint a
 * manual upload uses.
 */
import { computed, ref, watch } from "vue";

import {
  compareVersions,
  downloadFirmware,
  fetchRelease,
  uploadFirmware,
  type FirmwareRelease,
  type SystemInfo,
  type Values,
} from "../state/device";
import { toast } from "../state/toasts";

const props = defineProps<{ system: SystemInfo | null; values: Values }>();

const open = ref(false);
const release = ref<FirmwareRelease | null>(null);
const checking = ref(false);
const checkError = ref<string | null>(null);
const uploading = ref(false);
const firmwarePct = ref(0);

const updateUrl = computed(() => String(props.values.upd_url ?? "").trim());
const updateEnabled = computed(() => Boolean(props.values.upd_check) && updateUrl.value !== "");

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

watch(updateEnabled, (on) => (on ? void checkForUpdate() : (release.value = null)), {
  immediate: true,
});
watch(updateUrl, () => void checkForUpdate());

async function installRelease() {
  const target = release.value;
  if (!target) return;
  if (!confirm(`Install ${target.version} and restart the device?`)) return;
  uploading.value = true;
  firmwarePct.value = 0;
  try {
    const image = await downloadFirmware(target);
    await uploadFirmware(image, (f) => (firmwarePct.value = Math.round(f * 100)));
    toast.info(`${target.version} written, the device is restarting`);
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    uploading.value = false;
  }
}

async function onFirmwareChosen(e: Event) {
  const input = e.target as HTMLInputElement;
  const file = input.files?.[0];
  if (!file) return;
  if (!confirm(`Install ${file.name} and restart the device?`)) return;
  uploading.value = true;
  firmwarePct.value = 0;
  try {
    await uploadFirmware(file, (f) => (firmwarePct.value = Math.round(f * 100)));
    toast.info("Firmware written, the device is restarting");
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    uploading.value = false;
    input.value = "";
  }
}

/* The badge outline: a filling ring while uploading, accent when an update is
   available, a plain border otherwise. */
const ringStyle = computed(() => {
  if (uploading.value) {
    return {
      background: `conic-gradient(var(--accent) ${firmwarePct.value}%, var(--border) ${firmwarePct.value}%)`,
    };
  }
  if (updateAvailable.value) return { background: "var(--accent)" };
  return {};
});
</script>

<template>
  <div class="uw" v-if="system">
    <button
      type="button"
      class="uw-badge"
      :style="ringStyle"
      :title="updateAvailable ? 'Update available' : 'Firmware'"
      :aria-label="`Firmware ${system.version}${updateAvailable ? ', update available' : ''}`"
      @click="open = !open"
    >
      <span class="uw-inner mono">
        {{ uploading ? `${firmwarePct}%` : system.version }}
        <span v-if="updateAvailable && !uploading" class="uw-dot" aria-hidden="true" />
      </span>
    </button>

    <template v-if="open">
      <div class="uw-backdrop" @click="open = false" />
      <div class="uw-popup">
        <div class="uw-head">
          <h3>Firmware</h3>
          <button type="button" class="btn btn-sm btn-quiet" @click="open = false">Close</button>
        </div>

        <dl class="facts">
          <div class="fact"><dt>Version</dt><dd class="mono">{{ system.version }}</dd></div>
          <div class="fact"><dt>Built</dt><dd class="mono">{{ system.built }}</dd></div>
          <div class="fact"><dt>Running from</dt><dd class="mono">{{ system.partition }}</dd></div>
        </dl>

        <p v-if="!system.updatable" class="section-blocked">
          This firmware has a single app slot, so it cannot update itself.
        </p>
        <template v-else>
          <p class="setting-note">
            The image is written to the spare slot. If it fails to start, the device returns to
            this one on its own.
          </p>

          <div v-if="updateEnabled">
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
                {{ uploading ? `Installing ${firmwarePct}%...` : `Install ${release.version}` }}
              </button>
            </template>
            <button
              type="button"
              class="btn btn-sm btn-quiet"
              :disabled="checking || uploading"
              @click="checkForUpdate"
            >
              Check again
            </button>
          </div>

          <p class="setting-note">
            Or install a specific build by hand: download its <code>.bin</code> from the
            <a
              href="https://github.com/espkvm/espkvm/releases"
              target="_blank"
              rel="noreferrer"
              >releases page</a
            >
            and pick it below.
          </p>
          <label :class="['btn', 'btn-sm', { 'btn-disabled': uploading }]">
            {{ uploading ? `Uploading ${firmwarePct}%...` : "Install firmware..." }}
            <input
              type="file"
              accept=".bin"
              class="sr-only"
              :disabled="uploading"
              @change="onFirmwareChosen"
            />
          </label>
          <div
            v-if="uploading"
            class="progress"
            role="progressbar"
            :aria-valuenow="firmwarePct"
            aria-valuemin="0"
            aria-valuemax="100"
          >
            <div class="progress-fill" :style="{ width: firmwarePct + '%' }"></div>
          </div>
          <p v-if="uploading" class="setting-note">
            Keep this page open until it finishes and the device restarts.
          </p>
        </template>
      </div>
    </template>
  </div>
</template>

<style scoped>
.uw {
  position: relative;
}

/* The outlined badge. The button's own background is the "outline": an inner
   span sits on top with the surface colour, leaving a 2px ring that ringStyle
   can turn into a progress arc or an accent border. */
.uw-badge {
  padding: 2px;
  border: none;
  border-radius: var(--radius);
  background: var(--border);
  cursor: pointer;
  line-height: 0;
}

.uw-inner {
  display: inline-flex;
  align-items: center;
  gap: var(--space-1, 4px);
  padding: 2px 8px;
  border-radius: calc(var(--radius) - 2px);
  background: var(--bg-raised);
  color: var(--text);
  font-size: var(--text-sm);
  line-height: 1.4;
}

.uw-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--accent);
}

.uw-backdrop {
  position: fixed;
  inset: 0;
  z-index: 40;
}

.uw-popup {
  /* Fixed, not absolute: the status bar sets overflow-x, which makes overflow-y
     compute to auto and would clip a popup dropping below the bar. Fixed escapes
     that, positioned just under the bar at the right where the badge sits. */
  position: fixed;
  top: calc(var(--bar-height) + 6px);
  right: var(--space-3);
  z-index: 41;
  width: 320px;
  max-width: 90vw;
  max-height: 70vh;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: var(--space-3);
  padding: var(--space-4);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  background: var(--bg-raised);
  box-shadow: var(--shadow, 0 8px 24px rgba(0, 0, 0, 0.4));
}

.uw-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.uw-head h3 {
  margin: 0;
}
</style>
