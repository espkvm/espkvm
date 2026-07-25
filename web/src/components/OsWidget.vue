<script setup lang="ts">
/*
 * The guessed target OS as a rail button above Settings: the button shows the OS
 * icon, a click opens the raw USB fingerprint the guess was inferred from beside
 * the rail. Only present once a target is attached over USB and the guess is
 * something other than unknown. The `side` prop is the rail's edge so the popup
 * opens inward over the stage, matching the diagnostics widget below it.
 */
import { computed, ref } from "vue";

import OsIcon from "./OsIcon.vue";
import type { UsbProbe } from "../state/device";

const props = defineProps<{
  probe: UsbProbe | null;
  attached: boolean;
  side: "left" | "right";
}>();

const open = ref(false);

const OS_NAMES: Record<string, string> = {
  windows: "Windows",
  macos: "macOS",
  linux: "Linux",
  android: "Android",
};

const show = computed(
  () => props.attached && !!props.probe && props.probe.os !== "unknown",
);
const name = computed(() =>
  props.probe ? (OS_NAMES[props.probe.os] ?? props.probe.os) : "",
);
</script>

<template>
  <div class="ow" :data-side="side" v-if="show && probe">
    <button
      type="button"
      class="rail-btn ow-btn"
      :class="{ 'rail-btn-active': open }"
      :title="`Target looks like ${name} - click for the USB fingerprint`"
      :aria-label="`Target OS: ${name}`"
      @click="open = !open"
    >
      <OsIcon :os="probe.os" />
    </button>

    <template v-if="open">
      <div class="ow-backdrop" @click="open = false" />
      <div class="ow-popup">
        <div class="ow-head">
          <OsIcon :os="probe.os" />
          Looks like <strong>{{ name }}</strong>
        </div>
        <p>
          Inferred from how the target enumerated over USB. If that is wrong,
          send us this fingerprint so a later build can learn it:
        </p>
        <pre class="fingerprint">{{ probe.trace }}</pre>
      </div>
    </template>
  </div>
</template>

<style scoped>
.ow {
  position: relative;
}

.ow-btn :deep(.os-icon) {
  width: 18px;
  height: 18px;
}

.ow-backdrop {
  position: fixed;
  inset: 0;
  z-index: 40;
}

/* Same anchoring as the diagnostics widget: absolute, opening inward past the
   rail's edge (the rail sets no overflow, so it is not clipped). */
.ow-popup {
  position: absolute;
  bottom: 0;
  z-index: 41;
  width: min(340px, 80vw);
  padding: var(--space-4);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  background: var(--bg-raised);
  box-shadow: var(--shadow, 0 8px 24px rgba(0, 0, 0, 0.4));
}

.ow[data-side="left"] .ow-popup {
  left: calc(100% + 8px);
}

.ow[data-side="right"] .ow-popup {
  right: calc(100% + 8px);
}

.ow-head {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 15px;
  margin-bottom: 8px;
}

.ow-head :deep(.os-icon) {
  width: 22px;
  height: 22px;
  color: var(--text);
}

.ow-popup p {
  margin: 0 0 8px;
  color: var(--text-muted);
  font-size: 13px;
}
</style>
