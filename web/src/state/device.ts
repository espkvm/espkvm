/*
 * The device is the source of truth for configuration and for what the hardware
 * can actually do.
 *
 * Nothing here caches a preference in the browser: a KVM is reached from a
 * laptop today and a phone tomorrow, and the thing being configured is the
 * device. Capabilities come from the same place, so a control the hardware
 * cannot support is disabled with the device's own explanation rather than a
 * guess made in the UI.
 */

export type SettingType = "bool" | "int" | "enum" | "string";

export interface Setting {
  key: string;
  section: string;
  title: string;
  help?: string;
  type: SettingType;
  min?: number;
  max?: number;
  choices?: string[];
  maxLength?: number;
  default: number | string;
  /** Capability this setting depends on, if any. */
  requires?: string;
  /** Takes effect only after the device restarts. */
  reboot?: boolean;
}

export interface Capability {
  compiled: boolean;
  available: boolean;
  enabled: boolean;
  active: boolean;
  setting?: string;
  reason?: string;
}

export interface VideoStatus {
  signal: boolean;
  width: number;
  height: number;
  interlaced: boolean;
  fps: number;
  skippedFps: number;
  kbps: number;
  /** Mean time one frame took to encode. */
  encodeUs: number;
  /** Share of wall clock the encoder was busy. */
  encoderBusyPct: number;
  modeChanges: number;
  sysStatus: number;
  viewers: number;
  /** Codec currently running: "mjpeg", "h264", or "none" before the first frame. */
  codec: string;
}

export interface SystemInfo {
  project: string;
  version: string;
  built: string;
  idf: string;
  partition: string;
  /** A second app slot exists, so an update could be installed. */
  updatable: boolean;
  uptimeSeconds: number;
  heapFree: number;
  psramFree: number;
  /** 0 when the sensor is unavailable. */
  tempC: number;
}

export type Values = Record<string, number | string | boolean>;

const SETTINGS_URL = "/api/v1/settings";
const SCHEMA_URL = "/api/v1/settings/schema";
const CAPS_URL = "/api/capabilities";
const VIDEO_URL = "/api/v1/video/status";

/**
 * The device says "who is asking?" with a 401, and that is not the same thing
 * as being unreachable: sessions live in the device's memory, so every reboot
 * signs everyone out and a console left open overnight will meet this. Telling
 * the two apart is the difference between "sign in again" and an operator
 * hunting for a network fault that is not there.
 */
export class Unauthorized extends Error {
  constructor() {
    super("the session has ended");
    this.name = "Unauthorized";
  }
}

async function getJson<T>(url: string): Promise<T> {
  const res = await fetch(url, { cache: "no-store" });
  if (res.status === 401) throw new Unauthorized();
  if (!res.ok) throw new Error(`${url} returned ${res.status}`);
  return (await res.json()) as T;
}

export async function loadSchema(): Promise<Setting[]> {
  return getJson<Setting[]>(SCHEMA_URL);
}

export async function loadValues(): Promise<Values> {
  return getJson<Values>(SETTINGS_URL);
}

export async function loadCapabilities(): Promise<Record<string, Capability>> {
  return getJson<Record<string, Capability>>(CAPS_URL);
}

export async function loadSystemInfo(): Promise<SystemInfo> {
  return getJson<SystemInfo>("/api/v1/system/info");
}

export async function loadVideoStatus(): Promise<VideoStatus> {
  return getJson<VideoStatus>(VIDEO_URL);
}

/**
 * Write settings. The device validates and applies all or none, so a rejected
 * value never leaves the interface showing something the device is not doing.
 * @returns every setting as the device now sees it
 */
export async function saveSettings(patch: Values): Promise<Values> {
  const res = await fetch(SETTINGS_URL, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(patch),
  });
  if (res.status === 401) throw new Unauthorized();
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error((body as { error?: string }).error ?? `settings write failed (${res.status})`);
  }
  return body as Values;
}

export async function resetSettings(): Promise<Values> {
  const res = await fetch(`${SETTINGS_URL}/reset`, { method: "POST" });
  if (!res.ok) throw new Error(`reset failed (${res.status})`);
  return (await res.json()) as Values;
}

/**
 * Send a firmware image. The device writes it to the inactive slot and
 * restarts; if the new image never comes up, the bootloader returns to this
 * one, so the failure mode is a reboot rather than a dead device.
 */
export async function uploadFirmware(file: Blob): Promise<void> {
  const res = await fetch("/api/v1/system/update", {
    method: "POST",
    headers: { "Content-Type": "application/octet-stream" },
    body: file,
  });
  if (res.status === 401) throw new Unauthorized();
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error((body as { error?: string }).error ?? `update failed (${res.status})`);
  }
}

/**
 * A published build, as described by the manifest the project's CI writes.
 *
 * The check happens here, in the browser, and never on the device: a KVM sits
 * in networks that have no way out, and one that quietly talks to the internet
 * is not what belongs there. The browser fetches the image and hands it to the
 * device through the same endpoint as a manual upload.
 */
export interface FirmwareRelease {
  version: string;
  /** Absolute URL of the image, resolved against the manifest. */
  url: string;
  size?: number;
  released?: string;
  /** Where a human can read what changed. */
  notes?: string;
}

export async function fetchRelease(manifestUrl: string): Promise<FirmwareRelease> {
  const res = await fetch(manifestUrl, { cache: "no-store" });
  if (!res.ok) throw new Error(`the update manifest returned ${res.status}`);
  const body = (await res.json()) as {
    version?: string;
    file?: string;
    size?: number;
    released?: string;
    notes?: string;
  };
  if (!body.version || !body.file) throw new Error("the update manifest is missing a version");
  return {
    version: body.version,
    url: new URL(body.file, manifestUrl).href,
    size: body.size,
    released: body.released,
    notes: body.notes,
  };
}

/**
 * Compare two release versions, e.g. "v1.2.0" against "v1.10.0".
 *
 * @returns a negative number when @p a is older, 0 when they match, positive
 *          when @p a is newer, and null when either side is not a release at
 *          all - an untagged build reports the commit it came from, and no
 *          ordering between that and a version number is meaningful.
 */
export function compareVersions(a: string, b: string): number | null {
  const parse = (v: string): number[] | null => {
    /* Tags are written v1.2.3, and "v.1.2.3" is common enough to accept. */
    const m = /^v\.?(\d+)\.(\d+)(?:\.(\d+))?$/.exec(v.trim());
    return m ? [Number(m[1]), Number(m[2]), Number(m[3] ?? 0)] : null;
  };
  const left = parse(a);
  const right = parse(b);
  if (!left || !right) return null;
  for (let i = 0; i < 3; i++) {
    if (left[i] !== right[i]) return left[i] - right[i];
  }
  return 0;
}

/** Fetch the image itself, so it can be handed to the device. */
export async function downloadFirmware(release: FirmwareRelease): Promise<Blob> {
  const res = await fetch(release.url, { cache: "no-store" });
  if (!res.ok) throw new Error(`downloading ${release.version} returned ${res.status}`);
  return await res.blob();
}

/* ---- virtual media -------------------------------------------------------
 *
 * Images live on the microSD card; the device serves the selected one to the
 * target over USB. The card is the store, so the browser only lists, uploads
 * and deletes - it never holds an image itself.
 */

export interface StorageImage {
  name: string;
  size: number;
}

export interface StorageInfo {
  mounted: boolean;
  totalBytes: number;
  freeBytes: number;
  /** File name currently offered to the target, or "" when ejected. */
  active: string;
  images: StorageImage[];
  /** Whether the device can upload/delete; false when the card is read-only. */
  writable: boolean;
  /** Why writing is unavailable, when it is. */
  writeReason?: string;
}

export async function loadImages(): Promise<StorageInfo> {
  return getJson<StorageInfo>("/api/v1/storage/images");
}

/**
 * Stream a file to the card. Uses XMLHttpRequest, not fetch, for one reason:
 * an image is measured in gigabytes and the operator needs to see it move.
 * fetch gives no upload progress; XHR does.
 */
export function uploadImage(file: File, onProgress?: (fraction: number) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", `/api/v1/storage/upload?name=${encodeURIComponent(file.name)}`);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total);
    };
    xhr.onload = () => {
      if (xhr.status === 401) return reject(new Unauthorized());
      if (xhr.status >= 200 && xhr.status < 300) return resolve();
      let message = `upload failed (${xhr.status})`;
      try {
        const body = JSON.parse(xhr.responseText) as { error?: string };
        if (body.error) message = body.error;
      } catch {
        /* keep the status-code message */
      }
      reject(new Error(message));
    };
    xhr.onerror = () => reject(new Error("upload failed: the connection dropped"));
    xhr.send(file);
  });
}

export async function deleteImage(name: string): Promise<StorageInfo> {
  const res = await fetch(`/api/v1/storage/delete?name=${encodeURIComponent(name)}`, {
    method: "POST",
  });
  if (res.status === 401) throw new Unauthorized();
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error((body as { error?: string }).error ?? `delete failed (${res.status})`);
  }
  return body as StorageInfo;
}

/** A byte count as a short human string, e.g. 3.2 GB. */
export function formatBytes(n: number): string {
  if (n <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const i = Math.min(units.length - 1, Math.floor(Math.log(n) / Math.log(1024)));
  const value = n / Math.pow(1024, i);
  return `${i === 0 ? value : value.toFixed(1)} ${units[i]}`;
}

/** Resolve an enum setting to its name, e.g. mouse_mode -> "absolute". */
export function enumName(schema: Setting[], values: Values, key: string): string | null {
  const entry = schema.find((s) => s.key === key);
  if (!entry?.choices) return null;
  const index = Number(values[key]);
  return entry.choices[index] ?? null;
}

/** Index for an enum setting's name, for writing it back. */
export function enumIndex(schema: Setting[], key: string, name: string): number | null {
  const entry = schema.find((s) => s.key === key);
  if (!entry?.choices) return null;
  const i = entry.choices.indexOf(name);
  return i < 0 ? null : i;
}

/**
 * Why a setting cannot be changed right now, or null when it can.
 * The device's own wording is used so the interface never invents a reason.
 */
export function settingBlockedReason(
  setting: Setting,
  caps: Record<string, Capability>,
): string | null {
  if (!setting.requires) return null;
  const cap = caps[setting.requires];
  if (!cap) return null;
  if (!cap.compiled) return cap.reason ?? "not built into this firmware";
  if (!cap.available) return cap.reason ?? "not available on this hardware";
  return null;
}

export const SECTION_TITLES: Record<string, string> = {
  video: "Video",
  input: "Input",
  storage: "Virtual media",
  power: "Power",
  network: "Network",
  security: "Security",
  system: "System",
};

export const SECTION_ORDER = [
  "video",
  "input",
  "storage",
  "power",
  "network",
  "security",
  "system",
];
