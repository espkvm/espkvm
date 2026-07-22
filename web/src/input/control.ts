/*
 * The control channel to the device: protocol v2, defined in
 * components/kvm_web/http_server.c.
 *
 *   client -> device
 *     0x01 abs mouse   buttons, x:u16, y:u16 (0..32767), wheel:i8, pan:i8
 *     0x02 rel mouse   buttons, dx:i16, dy:i16, wheel:i8, pan:i8
 *     0x03 keyboard    modifier, six key usages
 *     0x04 consumer    usage:u16
 *     0x05 release all
 *     0x06 ping
 *   device -> client
 *     0x81 status      flags:u8 (bit0 target attached), leds:u8
 *     0x82 pong
 *
 * The device serves one control client at a time and drops whatever was held
 * down when the socket closes, so reconnecting is always safe.
 */

export const ABS_MAX = 32767;

const MSG_MOUSE_ABS = 0x01;
const MSG_MOUSE_REL = 0x02;
const MSG_KEYBOARD = 0x03;
const MSG_CONSUMER = 0x04;
const MSG_RELEASE_ALL = 0x05;
const MSG_PING = 0x06;
const MSG_STATUS = 0x81;

export interface TargetState {
  /** The target machine has enumerated our USB device. */
  attached: boolean;
  /** False while the control channel is down: the device has not said either
   *  way, and reporting "no target" then blames the wrong thing. */
  known: boolean;
  /** Lock-key LEDs as the target reports them: bit0 num, bit1 caps, bit2 scroll. */
  leds: number;
}

export type ConnectionState = "connecting" | "open" | "closed";

interface Handlers {
  onTarget(state: TargetState): void;
  onConnection(state: ConnectionState): void;
}

export class Control {
  #ws: WebSocket | null = null;
  #retry: number | null = null;
  #closed = false;
  #handlers: Handlers;
  #backoff = 500;

  constructor(handlers: Handlers) {
    this.#handlers = handlers;
    this.#connect();
  }

  get open(): boolean {
    return this.#ws?.readyState === WebSocket.OPEN;
  }

  #connect() {
    if (this.#closed) return;
    this.#handlers.onConnection("connecting");
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${proto}://${location.host}/ws`);
    ws.binaryType = "arraybuffer";
    this.#ws = ws;

    ws.onopen = () => {
      this.#backoff = 500;
      this.#handlers.onConnection("open");
      /* The device cannot push during the handshake, so ask for the current
         target state rather than showing blank indicators until something
         happens to change them. */
      this.#send(new Uint8Array([MSG_PING]));
    };

    ws.onmessage = (ev) => {
      if (!(ev.data instanceof ArrayBuffer)) return;
      const b = new Uint8Array(ev.data);
      if (b[0] === MSG_STATUS && b.length >= 3) {
        this.#handlers.onTarget({ attached: (b[1] & 1) !== 0, leds: b[2], known: true });
      }
    };

    ws.onerror = () => ws.close();

    ws.onclose = () => {
      this.#ws = null;
      this.#handlers.onConnection("closed");
      /* Not "the target is gone" - we simply stopped being told. */
      this.#handlers.onTarget({ attached: false, leds: 0, known: false });
      if (this.#closed) return;
      /* Back off so a device that is rebooting is not hammered, but stay
         responsive enough that a brief blip is invisible. The ceiling is high
         because the reason may not be temporary: a device that has signed this
         page out refuses the socket every time, and a page that keeps asking
         every few seconds for hours ends up unusable. */
      this.#retry = window.setTimeout(() => this.#connect(), this.#backoff);
      this.#backoff = Math.min(this.#backoff * 2, 30000);
    };
  }

  dispose() {
    this.#closed = true;
    if (this.#retry !== null) clearTimeout(this.#retry);
    this.#ws?.close();
    this.#ws = null;
  }

  #send(data: Uint8Array) {
    if (this.#ws?.readyState === WebSocket.OPEN) this.#ws.send(data.buffer as ArrayBuffer);
  }

  mouseAbsolute(buttons: number, x: number, y: number, wheel = 0, pan = 0) {
    const b = new Uint8Array(8);
    const dv = new DataView(b.buffer);
    dv.setUint8(0, MSG_MOUSE_ABS);
    dv.setUint8(1, buttons);
    dv.setUint16(2, clamp(x, 0, ABS_MAX), true);
    dv.setUint16(4, clamp(y, 0, ABS_MAX), true);
    dv.setInt8(6, clamp(wheel, -127, 127));
    dv.setInt8(7, clamp(pan, -127, 127));
    this.#send(b);
  }

  mouseRelative(buttons: number, dx: number, dy: number, wheel = 0, pan = 0) {
    const b = new Uint8Array(8);
    const dv = new DataView(b.buffer);
    dv.setUint8(0, MSG_MOUSE_REL);
    dv.setUint8(1, buttons);
    dv.setInt16(2, clamp(dx, -32768, 32767), true);
    dv.setInt16(4, clamp(dy, -32768, 32767), true);
    dv.setInt8(6, clamp(wheel, -127, 127));
    dv.setInt8(7, clamp(pan, -127, 127));
    this.#send(b);
  }

  keyboard(modifier: number, keys: number[]) {
    const b = new Uint8Array(8);
    b[0] = MSG_KEYBOARD;
    b[1] = modifier & 0xff;
    for (let i = 0; i < 6; i++) b[2 + i] = keys[i] ?? 0;
    this.#send(b);
  }

  consumer(usage: number) {
    const b = new Uint8Array(3);
    const dv = new DataView(b.buffer);
    dv.setUint8(0, MSG_CONSUMER);
    dv.setUint16(1, usage, true);
    this.#send(b);
  }

  releaseAll() {
    this.#send(new Uint8Array([MSG_RELEASE_ALL]));
  }
}

function clamp(v: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, Math.round(v)));
}
