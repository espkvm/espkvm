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
  /* Mouse buttons currently pressed on the target, so a lost up can be undone. */
  let buttonsHeld = 0;

  const control = new Control({
    onTarget: (t) => (target.value = t),
    onConnection: (c) => (connection.value = c),
  });
  onScopeDispose(() => control.dispose());

  /*
   * Lift everything held on the target, now, on the caller's stack - not on a
   * later tick. The device is the machine the operator is sitting at, and a
   * button left pressed there is the difference between a working mouse and a
   * dead one. The device's release clears the mouse buttons at its own last
   * position, so this does not send a coordinate - doing so would teleport the
   * target cursor to wherever this page last thought it was, which at start-up
   * is the top-left corner.
   */
  function releaseEverything() {
    held.clear();
    buttonsHeld = 0;
    control.releaseAll();
  }

  /* A backstop: whenever control is not held, nothing should be. This runs
     asynchronously, so the deliberate release paths below do not wait for it. */
  watchEffect(() => {
    if (opts.engaged.value) return;
    releaseEverything();
  });

  function mapToTarget(e: { clientX: number; clientY: number }) {
    const el = opts.surface.value;
    if (!el) return null;
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return null;

    /*
     * In "fit" the element fills the stage but the picture is letterboxed
     * inside it by object-fit: contain, so the element box is wider or taller
     * than the video. Mapping against the whole box would put the pointer into
     * the black bars - the cursor drifts on whichever axis is padded. Find the
     * rectangle the video actually occupies from its intrinsic size (a canvas
     * carries the frame resolution in width/height, an <img> in natural*), and
     * map into that. In "actual" the two aspects match, so this is a no-op.
     */
    const iw = (el as HTMLCanvasElement).width || (el as HTMLImageElement).naturalWidth || r.width;
    const ih =
      (el as HTMLCanvasElement).height || (el as HTMLImageElement).naturalHeight || r.height;
    const scale = Math.min(r.width / iw, r.height / ih);
    const shownW = iw * scale;
    const shownH = ih * scale;
    const padX = (r.width - shownW) / 2;
    const padY = (r.height - shownH) / 2;

    /* Coordinates are 0..32767, not pixels, so a resolution change on the
       target cannot shift the pointer mid-drag. */
    const x = ((e.clientX - r.left - padX) / shownW) * ABS_MAX;
    const y = ((e.clientY - r.top - padY) / shownH) * ABS_MAX;
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

    /*
     * A button-down only goes out while engaged, but a button-up must go out
     * whenever one is outstanding - even after control has been handed back,
     * even if the up lands outside the pane. Losing an up leaves the button
     * pressed on the target, which is the machine the operator is sitting at.
     */
    const onButton = (e: PointerEvent) => {
      const b = buttonsOf(e);
      if (!opts.engaged.value && !(buttonsHeld && b === 0)) return;
      buttonsHeld = b;
      const p = mapToTarget(e) ?? lastPos;
      e.preventDefault();
      control.mouseAbsolute(b, p.x, p.y);
    };

    /*
     * pointercancel fires instead of pointerup when the OS takes the pointer -
     * a gesture, a window drag, the compositor grabbing it. There is no up to
     * follow, so a button left down here stays down on the target.
     */
    const onCancel = () => {
      if (!buttonsHeld) return;
      buttonsHeld = 0;
      control.mouseAbsolute(0, lastPos.x, lastPos.y);
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

    /* A tab that is hidden mid-drag - switched away, minimised - may never see
       the up. Treat losing visibility as losing the pointer. */
    const onHidden = () => {
      if (document.visibilityState === "hidden") onCancel();
    };

    el.addEventListener("pointermove", onMove);
    el.addEventListener("pointerdown", onButton);
    el.addEventListener("wheel", onWheel, { passive: false });
    el.addEventListener("contextmenu", onContext);
    /* Release can land outside the pane after a drag. */
    document.addEventListener("pointerup", onButton);
    document.addEventListener("pointercancel", onCancel);
    document.addEventListener("visibilitychange", onHidden);

    onCleanup(() => {
      el.removeEventListener("pointermove", onMove);
      el.removeEventListener("pointerdown", onButton);
      el.removeEventListener("wheel", onWheel);
      el.removeEventListener("contextmenu", onContext);
      document.removeEventListener("pointerup", onButton);
      document.removeEventListener("pointercancel", onCancel);
      document.removeEventListener("visibilitychange", onHidden);
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
        /* Release synchronously, before disengaging - not through the async
           watchEffect, which could fire after the channel has moved on. */
        releaseEverything();
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
      releaseEverything();
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
