<script setup lang="ts">
/*
 * The way in.
 *
 * Two states in one view: signing in, and - when the device is still on the
 * password it shipped with - changing it before anything else is allowed. The
 * second is not a nag that can be dismissed: a KVM left on its default
 * password is a keyboard plugged into someone else's machine, offered to
 * whoever finds it.
 */
import { computed, ref } from "vue";

import { changePassword, login } from "../state/auth";
import Icon from "./Icon.vue";

const props = defineProps<{ user: string; mustChange: boolean }>();
const emit = defineEmits<{ authenticated: []; changed: [] }>();

const password = ref("");
const username = ref(props.user || "admin");
const nextPassword = ref("");
const confirmPassword = ref("");
const busy = ref(false);
const error = ref<string | null>(null);

const mismatch = computed(
  () => confirmPassword.value.length > 0 && nextPassword.value !== confirmPassword.value,
);
const tooShort = computed(() => nextPassword.value.length > 0 && nextPassword.value.length < 8);

async function submitLogin() {
  busy.value = true;
  error.value = null;
  try {
    const mustChange = await login(username.value, password.value);
    if (!mustChange) password.value = "";
    emit("authenticated");
  } catch (err) {
    error.value = err instanceof Error ? err.message : String(err);
  } finally {
    busy.value = false;
  }
}

async function submitChange() {
  if (mismatch.value || tooShort.value) return;
  busy.value = true;
  error.value = null;
  try {
    await changePassword(password.value, nextPassword.value);
    password.value = "";
    nextPassword.value = "";
    confirmPassword.value = "";
    emit("changed");
  } catch (err) {
    error.value = err instanceof Error ? err.message : String(err);
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="login">
    <form class="login-card" @submit.prevent="mustChange ? submitChange() : submitLogin()">
      <svg class="login-mark" viewBox="6 15 51 33" width="68" height="44" aria-hidden="true">
        <g fill="currentColor">
          <rect x="6" y="15" width="3" height="3" />
          <rect x="9" y="15" width="3" height="3" />
          <rect x="12" y="15" width="3" height="3" />
          <rect x="15" y="15" width="3" height="3" />
          <rect x="18" y="15" width="3" height="3" />
          <rect x="6" y="18" width="3" height="3" />
          <rect x="6" y="21" width="3" height="3" />
          <rect x="9" y="21" width="3" height="3" />
          <rect x="12" y="21" width="3" height="3" />
          <rect x="15" y="21" width="3" height="3" />
          <rect x="6" y="24" width="3" height="3" />
          <rect x="6" y="27" width="3" height="3" />
          <rect x="9" y="27" width="3" height="3" />
          <rect x="12" y="27" width="3" height="3" />
          <rect x="15" y="27" width="3" height="3" />
          <rect x="18" y="27" width="3" height="3" />
          <rect x="24" y="15" width="3" height="3" />
          <rect x="27" y="15" width="3" height="3" />
          <rect x="30" y="15" width="3" height="3" />
          <rect x="33" y="15" width="3" height="3" />
          <rect x="36" y="15" width="3" height="3" />
          <rect x="24" y="18" width="3" height="3" />
          <rect x="24" y="21" width="3" height="3" />
          <rect x="27" y="21" width="3" height="3" />
          <rect x="30" y="21" width="3" height="3" />
          <rect x="33" y="21" width="3" height="3" />
          <rect x="36" y="21" width="3" height="3" />
          <rect x="36" y="24" width="3" height="3" />
          <rect x="24" y="27" width="3" height="3" />
          <rect x="27" y="27" width="3" height="3" />
          <rect x="30" y="27" width="3" height="3" />
          <rect x="33" y="27" width="3" height="3" />
          <rect x="36" y="27" width="3" height="3" />
          <rect x="42" y="15" width="3" height="3" />
          <rect x="45" y="15" width="3" height="3" />
          <rect x="48" y="15" width="3" height="3" />
          <rect x="51" y="15" width="3" height="3" />
          <rect x="54" y="15" width="3" height="3" />
          <rect x="42" y="18" width="3" height="3" />
          <rect x="54" y="18" width="3" height="3" />
          <rect x="42" y="21" width="3" height="3" />
          <rect x="45" y="21" width="3" height="3" />
          <rect x="48" y="21" width="3" height="3" />
          <rect x="51" y="21" width="3" height="3" />
          <rect x="54" y="21" width="3" height="3" />
          <rect x="42" y="24" width="3" height="3" />
          <rect x="42" y="27" width="3" height="3" />
          <rect x="6" y="33" width="3" height="3" />
          <rect x="18" y="33" width="3" height="3" />
          <rect x="6" y="36" width="3" height="3" />
          <rect x="15" y="36" width="3" height="3" />
          <rect x="6" y="39" width="3" height="3" />
          <rect x="9" y="39" width="3" height="3" />
          <rect x="12" y="39" width="3" height="3" />
          <rect x="6" y="42" width="3" height="3" />
          <rect x="15" y="42" width="3" height="3" />
          <rect x="6" y="45" width="3" height="3" />
          <rect x="18" y="45" width="3" height="3" />
          <rect x="24" y="33" width="3" height="3" />
          <rect x="36" y="33" width="3" height="3" />
          <rect x="24" y="36" width="3" height="3" />
          <rect x="36" y="36" width="3" height="3" />
          <rect x="24" y="39" width="3" height="3" />
          <rect x="36" y="39" width="3" height="3" />
          <rect x="27" y="42" width="3" height="3" />
          <rect x="33" y="42" width="3" height="3" />
          <rect x="30" y="45" width="3" height="3" />
          <rect x="42" y="33" width="3" height="3" />
          <rect x="54" y="33" width="3" height="3" />
          <rect x="42" y="36" width="3" height="3" />
          <rect x="45" y="36" width="3" height="3" />
          <rect x="51" y="36" width="3" height="3" />
          <rect x="54" y="36" width="3" height="3" />
          <rect x="42" y="39" width="3" height="3" />
          <rect x="48" y="39" width="3" height="3" />
          <rect x="54" y="39" width="3" height="3" />
          <rect x="42" y="42" width="3" height="3" />
          <rect x="54" y="42" width="3" height="3" />
          <rect x="42" y="45" width="3" height="3" />
          <rect x="54" y="45" width="3" height="3" />
        </g>
      </svg>

      <template v-if="!mustChange">
        <h1>Sign in</h1>
        <label class="field">
          <span>Username</span>
          <input v-model="username" type="text" autocomplete="username" autocapitalize="off" />
        </label>
        <label class="field">
          <span>Password</span>
          <input
            v-model="password"
            type="password"
            autocomplete="current-password"
            autofocus
          />
        </label>
      </template>

      <template v-else>
        <h1>Choose a password</h1>
        <p class="muted">
          This device is still using the password it shipped with. Everything else waits until
          that changes.
        </p>
        <label class="field">
          <span>New password</span>
          <input v-model="nextPassword" type="password" autocomplete="new-password" autofocus />
        </label>
        <label class="field">
          <span>Repeat it</span>
          <input v-model="confirmPassword" type="password" autocomplete="new-password" />
        </label>
        <p v-if="tooShort" class="login-hint">At least 8 characters.</p>
        <p v-else-if="mismatch" class="login-hint">The two do not match.</p>
      </template>

      <p v-if="error" class="login-error">
        <Icon name="warning" :size="16" />
        {{ error }}
      </p>

      <button
        type="submit"
        class="btn btn-primary"
        :disabled="busy || (mustChange && (tooShort || mismatch || !nextPassword))"
      >
        {{ busy ? "Working..." : mustChange ? "Set password" : "Sign in" }}
      </button>

      <p v-if="mustChange" class="muted login-foot">
        You will be asked to sign in again with the new password.
      </p>
    </form>
  </div>
</template>
