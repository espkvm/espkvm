<script setup lang="ts">
/*
 * A small monochrome mark for the detected target OS. Inline SVG so the console
 * stays self-contained (no external icons); drawn in currentColor. These are
 * generic family marks used only to label which OS the target looks like.
 */
defineProps<{ os: string }>();
</script>

<template>
  <!-- Windows: four panes -->
  <svg v-if="os === 'windows'" class="os-icon" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
    <rect x="3" y="3" width="8" height="8" rx="0.5" />
    <rect x="13" y="3" width="8" height="8" rx="0.5" />
    <rect x="3" y="13" width="8" height="8" rx="0.5" />
    <rect x="13" y="13" width="8" height="8" rx="0.5" />
  </svg>

  <!-- macOS: apple -->
  <svg v-else-if="os === 'macos'" class="os-icon" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
    <path d="M17.6 12.7c0-2.2 1.8-3.2 1.9-3.3-1-1.5-2.6-1.7-3.2-1.7-1.4-.1-2.6.8-3.3.8-.7 0-1.7-.8-2.8-.8-1.5 0-2.8.8-3.6 2.2-1.5 2.6-.4 6.5 1.1 8.6.7 1 1.5 2.2 2.6 2.1 1 0 1.4-.7 2.7-.7 1.2 0 1.6.7 2.7.6 1.1 0 1.8-1 2.5-2 .8-1.2 1.1-2.3 1.1-2.4-.1 0-2.1-.8-2.1-3.2z" />
    <path d="M15.4 6.2c.6-.7 1-1.7.9-2.7-.9 0-2 .6-2.6 1.3-.6.6-1.1 1.6-.9 2.6 1 .1 2-.5 2.6-1.2z" />
  </svg>

  <!-- Linux: penguin -->
  <svg v-else-if="os === 'linux'" class="os-icon" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
    <path d="M12 2.6c-2.4 0-3.7 1.9-3.7 4.6 0 1.6.1 2.6-.7 3.9-.9 1.4-2.5 3-3.2 4.9-.4 1 .1 1.6.9 1.4.3.7 1 1.3 2 1.6 1.9.6 3.3.6 4.7.6s2.8 0 4.7-.6c1-.3 1.7-.9 2-1.6.8.2 1.3-.4.9-1.4-.7-1.9-2.3-3.5-3.2-4.9-.8-1.3-.7-2.3-.7-3.9 0-2.7-1.3-4.6-3.7-4.6z" />
    <ellipse cx="12" cy="15.4" rx="2.4" ry="3.1" fill="#fff" opacity="0.9" />
    <circle cx="10.4" cy="7.2" r="1.1" fill="#fff" />
    <circle cx="13.6" cy="7.2" r="1.1" fill="#fff" />
    <circle cx="10.5" cy="7.3" r="0.5" fill="#0e1116" />
    <circle cx="13.5" cy="7.3" r="0.5" fill="#0e1116" />
    <path d="M11 9.1l1 .9 1-.9-1-.7z" fill="#e3a33c" />
  </svg>

  <!-- Android: robot head -->
  <svg v-else-if="os === 'android'" class="os-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true">
    <path d="M6 12a6 6 0 0 1 12 0v6H6z" fill="currentColor" stroke="none" />
    <line x1="8" y1="5.5" x2="9.5" y2="8" />
    <line x1="16" y1="5.5" x2="14.5" y2="8" />
    <circle cx="9.6" cy="11.5" r="0.9" fill="var(--bg-raised, #161b22)" stroke="none" />
    <circle cx="14.4" cy="11.5" r="0.9" fill="var(--bg-raised, #161b22)" stroke="none" />
  </svg>

  <!-- unknown: a plain monitor -->
  <svg v-else class="os-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
    <rect x="3" y="4" width="18" height="12" rx="2" />
    <path d="M9 20h6M12 16v4" />
  </svg>
</template>

<style scoped>
.os-icon {
  width: 1.1em;
  height: 1.1em;
  display: inline-block;
  vertical-align: -0.15em;
}
</style>
