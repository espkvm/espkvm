/* Transient notices, shared by every component without prop plumbing. */

import { reactive, readonly } from "vue";

export type ToastKind = "info" | "error";

interface Toast {
  id: number;
  kind: ToastKind;
  text: string;
}

const list = reactive<Toast[]>([]);
let nextId = 1;

export const toasts = readonly(list);

export function dismissToast(id: number) {
  const i = list.findIndex((t) => t.id === id);
  if (i >= 0) list.splice(i, 1);
}

function push(kind: ToastKind, text: string) {
  const id = nextId++;
  list.push({ id, kind, text });
  if (list.length > 4) list.shift();
  /* Errors stay: an operator who missed one has no other way to learn what
     the device refused. */
  if (kind !== "error") setTimeout(() => dismissToast(id), 4000);
}

export const toast = {
  info: (text: string) => push("info", text),
  error: (text: string) => push("error", text),
};
