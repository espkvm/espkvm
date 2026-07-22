<script setup lang="ts">
/*
 * alert() blocks the page, and a blocked page cannot forward a keystroke or
 * release a held modifier - on a remote console that is worse than the message
 * being missed. Errors stay until dismissed; confirmations fade.
 */
import { toasts, dismissToast } from "../state/toasts";
import Icon from "./Icon.vue";
</script>

<template>
  <div class="toasts" role="status" aria-live="polite">
    <div v-for="t in toasts" :key="t.id" :class="['toast', `toast-${t.kind}`]">
      <span class="toast-icon">
        <Icon :name="t.kind === 'error' ? 'warning' : 'info'" :size="15" />
      </span>
      <span class="toast-text">{{ t.text }}</span>
      <button type="button" class="toast-close" aria-label="Dismiss" @click="dismissToast(t.id)">
        <Icon name="close" :size="13" />
      </button>
    </div>
  </div>
</template>
