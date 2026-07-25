<script setup lang="ts">
/*
 * Live diagnostics as a rail button above Settings: the button carries the chip
 * temperature at a glance (the one number that says "healthy" or "in trouble"),
 * and a click opens the rest - memory and uptime - in a popup beside the rail.
 * The `side` prop is the rail's edge, so the popup always opens inward, over the
 * stage, whichever side the rail is on.
 */
import { computed, ref } from "vue";

import type { SystemInfo } from "../state/device";

const props = defineProps<{ system: SystemInfo | null; side: "left" | "right" }>();

const open = ref(false);

const tempLabel = computed(() => {
  const t = props.system?.tempC ?? 0;
  return t > 0 ? `${Math.round(t)}` : "";
});

/* A single coarse unit is enough at a glance - minutes, then hours, days,
   weeks, months, years - so the label stays two or three characters wide. */
const uptimeLabel = computed(() => {
  const s = props.system?.uptimeSeconds ?? 0;
  if (s < 3600) return `${Math.max(1, Math.floor(s / 60))}min`;
  if (s < 86400) return `${Math.floor(s / 3600)}h`;
  if (s < 604800) return `${Math.floor(s / 86400)}d`;
  if (s < 2592000) return `${Math.floor(s / 604800)}w`;
  if (s < 31536000) return `${Math.floor(s / 2592000)}m`;
  return `${Math.floor(s / 31536000)}y`;
});
</script>

<template>
  <div class="dw" :data-side="side" v-if="system">
    <button
      type="button"
      class="rail-btn dw-btn"
      :class="{ 'rail-btn-active': open }"
      :title="`Diagnostics${tempLabel ? ` - ${tempLabel} C` : ''}, up ${uptimeLabel}`"
      aria-label="Diagnostics"
      @click="open = !open"
    >
      <span v-if="tempLabel" class="dw-temp mono">{{ tempLabel }}&deg;</span>
      <span class="dw-up mono">{{ uptimeLabel }}</span>
    </button>

    <template v-if="open">
      <div class="dw-backdrop" @click="open = false" />
      <div class="dw-popup">
        <div class="dw-head">
          <h3>Diagnostics</h3>
          <button type="button" class="btn btn-sm btn-quiet" @click="open = false">Close</button>
        </div>
        <dl class="facts">
          <div v-if="system.tempC > 0" class="fact">
            <dt>Chip temperature</dt>
            <dd class="mono">{{ system.tempC.toFixed(1) }}&deg;C</dd>
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
          <div class="fact"><dt>ESP-IDF</dt><dd class="mono">{{ system.idf }}</dd></div>
        </dl>
      </div>
    </template>
  </div>
</template>

<style scoped>
.dw {
  position: relative;
}

.dw-btn {
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 1px;
  height: auto;
  padding: 4px 0;
  text-align: center;
}

.dw-temp {
  font-size: 12px;
  line-height: 1.1;
}

.dw-up {
  font-size: 10px;
  line-height: 1.1;
  color: var(--text-faint);
}

.dw-backdrop {
  position: fixed;
  inset: 0;
  z-index: 40;
}

/* Anchored to the button and opening inward (away from the rail's edge); the
   rail sets no overflow, so an absolute popup escapes it over the stage. */
.dw-popup {
  position: absolute;
  bottom: 0;
  z-index: 41;
  width: 300px;
  max-width: 80vw;
  display: flex;
  flex-direction: column;
  gap: var(--space-3);
  padding: var(--space-4);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  background: var(--bg-raised);
  box-shadow: var(--shadow, 0 8px 24px rgba(0, 0, 0, 0.4));
}

.dw[data-side="left"] .dw-popup {
  left: calc(100% + 8px);
}

.dw[data-side="right"] .dw-popup {
  right: calc(100% + 8px);
}

.dw-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.dw-head h3 {
  margin: 0;
}
</style>
