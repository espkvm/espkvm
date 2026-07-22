/*
 * Turns browser events into HID reports for the target.
 *
 * Everything here is gated on `engaged`. A pointer that drifts over the video
 * pane is recoverable; keystrokes leaking into a remote machine are not, so
 * the keyboard is never captured without a deliberate click - in either
 * pointer mode.
 */

import { onScopeDispose, ref, watchEffect, type Ref } from "vue";

import { ABS_MAX, Control, type ConnectionState, type TargetState } from "./control";
import { isModifierCode, modifierMask, usageForCode } from "./keymap";

export interface InputOptions {
  engaged: Ref<boolean>;
  /** From the device's ptr_engage setting. */
  engageMode: Ref<"click" | "hover">;
  /** From the device's mouse_mode setting. */
  pointerMode: Ref<"absolute" | "relative" | "auto">;
  invertScroll: Ref<boolean>;
  /** Element showing the target's screen; pointer coordinates map onto it. */
  surface: Ref<HTMLElement | null>;
  onDisengage(): void;
}

const MAX_KEYS = 6;

export function useInput(opts: InputOptions) {
  const target = ref<TargetState>({ attached: false, leds: 0, known: false });
  const connection = ref<ConnectionState>("connecting");
  const held = new Set<number>();
  const lastPos = { x: 0, y: 0 };

  const control = new Control({
    onTarget: (t) => (target.value = t),
    onConnection: (c) => (connection.value = c),
  });
  onScopeDispose(() => control.dispose());

  /* Never leave a key down on the target when control ends: nobody is left to
     lift it. */
  watchEffect(() => {
    if (opts.engaged.value) return;
    held.clear();
    control.releaseAll();
  });

  function mapToTarget(e: { clientX: number; clientY: number }) {
    const el = opts.surface.value;
    if (!el) return null;
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return null;
    /* Coordinates are 0..32767, not pixels, so a resolution change on the
       target cannot shift the pointer mid-drag. */
    const x = ((e.clientX - r.left) / r.width) * ABS_MAX;
    const y = ((e.clientY - r.top) / r.height) * ABS_MAX;
    if (x < 0 || y < 0 || x > ABS_MAX || y > ABS_MAX) return null;
    return { x, y };
  }

  function buttonsOf(e: { buttons: number }) {
    let b = 0;
    if (e.buttons & 1) b |= 1;
    if (e.buttons & 2) b |= 2;
    if (e.buttons & 4) b |= 4;
    return b;
  }

  /* ---- pointer ---------------------------------------------------------- */

  /*
   * The handlers read the refs when they fire rather than closing over them.
   * Capturing the values instead means a press and its release can be judged
   * by different closures: clicking to take control sends the button down,
   * then the release is swallowed because the listener still believes control
   * was not held - and the target is left in a drag that never ends.
   */
  watchEffect((onCleanup) => {
    const el = opts.surface.value;
    if (!el) return;

    const relative = () => opts.pointerMode.value === "relative";
    const tracking = () =>
      opts.engaged.value || (opts.engageMode.value === "hover" && !relative());

    const onMove = (e: PointerEvent) => {
      if (!tracking()) return;
      const p = mapToTarget(e);
      if (!p) return;
      lastPos.x = p.x;
      lastPos.y = p.y;
      if (relative() && opts.engaged.value) {
        control.mouseRelative(buttonsOf(e), e.movementX || 0, e.movementY || 0);
      } else {
        control.mouseAbsolute(buttonsOf(e), p.x, p.y);
      }
    };

    const onButton = (e: PointerEvent) => {
      if (!opts.engaged.value) return;
      const p = mapToTarget(e) ?? lastPos;
      e.preventDefault();
      control.mouseAbsolute(buttonsOf(e), p.x, p.y);
    };

    const onWheel = (e: WheelEvent) => {
      if (!tracking()) return;
      e.preventDefault();
      const clicks = Math.round(-e.deltaY / 100) * (opts.invertScroll.value ? -1 : 1);
      if (clicks === 0) return;
      const p = mapToTarget(e) ?? lastPos;
      control.mouseAbsolute(0, p.x, p.y, clicks);
    };

    const onContext = (e: Event) => e.preventDefault();

    el.addEventListener("pointermove", onMove);
    el.addEventListener("pointerdown", onButton);
    el.addEventListener("wheel", onWheel, { passive: false });
    el.addEventListener("contextmenu", onContext);
    /* Release can land outside the pane after a drag. */
    document.addEventListener("pointerup", onButton);

    onCleanup(() => {
      el.removeEventListener("pointermove", onMove);
      el.removeEventListener("pointerdown", onButton);
      el.removeEventListener("wheel", onWheel);
      el.removeEventListener("contextmenu", onContext);
      document.removeEventListener("pointerup", onButton);
    });
  });

  /* ---- keyboard --------------------------------------------------------- */

  watchEffect((onCleanup) => {
    if (!opts.engaged.value) return;

    const send = (e: KeyboardEvent) =>
      control.keyboard(modifierMask(e), [...held].slice(0, MAX_KEYS));

    const onDown = (e: KeyboardEvent) => {
      /* Esc hands control back rather than reaching the target; the macro bar
         exists so it can still be sent deliberately. */
      if (e.key === "Escape") {
        e.preventDefault();
        opts.onDisengage();
        return;
      }
      e.preventDefault();
      if (!isModifierCode(e.code)) {
        const usage = usageForCode(e.code);
        if (usage) held.add(usage);
      }
      send(e);
    };

    const onUp = (e: KeyboardEvent) => {
      e.preventDefault();
      const usage = usageForCode(e.code);
      if (usage) held.delete(usage);
      send(e);
    };

    /* Losing focus while a key is down would strand it on the target. */
    const onBlur = () => {
      held.clear();
      control.releaseAll();
      opts.onDisengage();
    };

    window.addEventListener("keydown", onDown, true);
    window.addEventListener("keyup", onUp, true);
    window.addEventListener("blur", onBlur);

    onCleanup(() => {
      window.removeEventListener("keydown", onDown, true);
      window.removeEventListener("keyup", onUp, true);
      window.removeEventListener("blur", onBlur);
    });
  });

  /**
   * The engaging click reaches the target too - swallowing it would cost the
   * operator a click on every interaction. Its release comes from the normal
   * pointerup handler, which by then sees control as held.
   */
  function engageFromPointer(e: PointerEvent) {
    const p = mapToTarget(e);
    if (!p) return;
    lastPos.x = p.x;
    lastPos.y = p.y;
    control.mouseAbsolute(buttonsOf(e), p.x, p.y);
  }

  return { target, connection, control, engageFromPointer };
}
