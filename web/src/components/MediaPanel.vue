<script setup lang="ts">
/*
 * Virtual media, as its own panel: the list of what the target can boot from -
 * the on-flash rescue image and the files on the microSD card - with the active
 * one picked here rather than buried in settings. Choosing writes the msc_image
 * setting; whether the drive is exposed at all, and its type, stay in Settings.
 *
 * The card is read-only on this board (its writes are unreliable), so uploads to
 * it are shown disabled with the reason; the flash rescue slot, whose writes are
 * reliable, can be uploaded to from here.
 */
import { onMounted, ref } from "vue";

import {
  deleteImage,
  formatBytes,
  loadImages,
  saveSettings,
  uploadImage,
  uploadRescue,
  RESCUE_MEDIUM,
  type StorageInfo,
  type Values,
} from "../state/device";
import { toast } from "../state/toasts";

const emit = defineEmits<{ (e: "values", v: Values): void }>();

const storage = ref<StorageInfo | null>(null);
const loadingImages = ref(false);
const uploadingImage = ref(false);
const uploadPct = ref(0);
const uploadingRescue = ref(false);
const rescuePct = ref(0);

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

/* The panel is only mounted when the operator opens it, so this reads the card
   on open rather than on every console load. */
onMounted(() => void refreshImages());

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

async function onRescueChosen(e: Event) {
  const input = e.target as HTMLInputElement;
  const file = input.files?.[0];
  if (!file) return;
  const cap = storage.value?.rescue?.capacityBytes ?? 0;
  if (cap && file.size > cap) {
    toast.error(`${file.name} is larger than the ${formatBytes(cap)} rescue partition`);
    input.value = "";
    return;
  }
  uploadingRescue.value = true;
  rescuePct.value = 0;
  try {
    storage.value = await uploadRescue(file, (f) => (rescuePct.value = Math.round(f * 100)));
    toast.info(`Rescue image written (${file.name})`);
  } catch (err) {
    toast.error(err instanceof Error ? err.message : String(err));
  } finally {
    uploadingRescue.value = false;
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
</script>

<template>
  <div class="media-panel">
    <p v-if="loadingImages && !storage" class="setting-note">Reading media...</p>
    <p
      v-else-if="storage && !storage.mounted && !storage.rescue?.supported"
      class="section-blocked"
    >
      No microSD card and no built-in rescue partition. Insert a card formatted FAT32 with your
      boot images copied on (up to 4&nbsp;GB per file).
    </p>
    <template v-else-if="storage">
      <p class="setting-note">
        The chosen medium is served to the target; turn on <em>Expose virtual media</em> under
        Settings for it to appear.
      </p>

      <ul class="image-list">
        <li
          v-if="storage.rescue?.supported"
          :class="['image-row', { 'image-active': storage.active === RESCUE_MEDIUM }]"
        >
          <label class="image-pick">
            <input
              type="radio"
              name="active-image"
              :checked="storage.active === RESCUE_MEDIUM"
              :disabled="!storage.rescue.hasImage"
              @change="selectImage(RESCUE_MEDIUM)"
            />
            <span class="image-name">Rescue image</span>
            <span class="muted">
              {{
                storage.rescue.hasImage
                  ? "on flash"
                  : `empty, up to ${formatBytes(storage.rescue.capacityBytes)}`
              }}
            </span>
          </label>
          <label :class="['btn', 'btn-sm', 'btn-quiet', { 'btn-disabled': uploadingRescue }]">
            {{
              uploadingRescue
                ? `${rescuePct}%...`
                : storage.rescue.hasImage
                  ? "Replace..."
                  : "Upload..."
            }}
            <input type="file" class="sr-only" :disabled="uploadingRescue" @change="onRescueChosen" />
          </label>
        </li>

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
        <li v-if="storage.mounted && storage.images.length === 0" class="muted image-empty">
          No images on the card yet. Upload one below.
        </li>
      </ul>

      <div
        v-if="uploadingRescue"
        class="progress"
        role="progressbar"
        :aria-valuenow="rescuePct"
        aria-valuemin="0"
        aria-valuemax="100"
      >
        <div class="progress-fill" :style="{ width: rescuePct + '%' }"></div>
      </div>

      <p v-if="storage.rescue?.supported" class="setting-note">
        Need a rescue image? A small one fits the flash slot -
        <a href="https://netboot.xyz" target="_blank" rel="noreferrer">netboot.xyz</a>
        boots a menu of rescue systems and installers over the network. Download it, then use
        Upload above.
      </p>

      <label class="image-pick image-eject">
        <input
          type="radio"
          name="active-image"
          :checked="!storage.active"
          @change="selectImage('')"
        />
        <span>Eject - offer the target no medium</span>
      </label>

      <template v-if="storage.mounted">
        <p class="setting-note">
          {{ formatBytes(storage.freeBytes) }} free of {{ formatBytes(storage.totalBytes) }} on the
          card.
        </p>
        <p v-if="!storage.writable" class="setting-note setting-note-blocked">
          {{ storage.writeReason ?? "The card is read-only on this device." }}
          Format it FAT32, one file up to 4&nbsp;GB.
        </p>
        <label
          v-if="storage.writable"
          :class="['btn', 'btn-sm', { 'btn-disabled': uploadingImage }]"
        >
          {{ uploadingImage ? `Uploading ${uploadPct}%...` : "Upload card image..." }}
          <input type="file" class="sr-only" :disabled="uploadingImage" @change="onImageChosen" />
        </label>
        <div
          v-if="uploadingImage"
          class="progress"
          role="progressbar"
          :aria-valuenow="uploadPct"
          aria-valuemin="0"
          aria-valuemax="100"
        >
          <div class="progress-fill" :style="{ width: uploadPct + '%' }"></div>
        </div>
      </template>
    </template>
  </div>
</template>
