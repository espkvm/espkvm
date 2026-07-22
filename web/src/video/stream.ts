/*
 * Receives video over the WebSocket channel and hands each frame to the caller
 * as something a canvas can draw.
 *
 * Two payloads arrive on the same channel and the device may switch between
 * them while a viewer is connected, so the type is read per frame:
 *
 *   JPEG   decoded with createImageBitmap; works everywhere
 *   H.264  decoded with WebCodecs; Annex-B, SPS and PPS in front of every
 *          keyframe, so no side-channel configuration is needed
 *
 * The device also still serves the old multipart stream, and this falls back to
 * it when the channel produces nothing: an operator locked out of the picture
 * because a transport changed is a worse outcome than an older transport. That
 * fallback exists for JPEG only - the multipart endpoint cannot carry H.264 -
 * so a browser that cannot decode H.264 is told so instead of being left with
 * an empty rectangle.
 *
 * Frame layout is defined in components/kvm_web/http_server.c.
 */

const HEADER_LEN = 12;
const MAGIC = 0x4b;
const TYPE_JPEG = 1;
const TYPE_H264 = 2;

/** How long to wait for the first frame before deciding the channel is dead. */
const FIRST_FRAME_TIMEOUT_MS = 2500;

/** Anything a canvas can draw and that must be released afterwards. */
export type Drawable = (ImageBitmap | VideoFrame) & { close(): void };

export interface FrameInfo {
  image: Drawable;
  width: number;
  height: number;
  sequence: number;
  /** Device milliseconds when the frame was sent. */
  pts: number;
}

interface Handlers {
  onFrame(frame: FrameInfo): void;
  /** The channel produced nothing usable; the caller should show the fallback. */
  onUnavailable(reason: string): void;
  /** The stream is H.264 and this browser cannot play it. No fallback exists. */
  onCodecError(reason: string): void;
}

/**
 * Which Annex-B NAL units are in a frame, as far as we need to know: whether it
 * can be decoded on its own, and the sequence parameter set that says which
 * H.264 profile and level the decoder must support.
 */
function inspectAnnexB(data: Uint8Array): { keyframe: boolean; sps: Uint8Array | null } {
  let keyframe = false;
  let sps: Uint8Array | null = null;
  let zeros = 0;
  for (let i = 0; i < data.length; i++) {
    const byte = data[i];
    if (byte === 0) {
      zeros++;
      continue;
    }
    if (byte === 1 && zeros >= 2) {
      const nalType = data[i + 1] & 0x1f;
      if (nalType === 7 && !sps) sps = data.subarray(i + 1);
      /* 5 is an IDR slice, 1 a non-IDR one; the first slice settles it. */
      if (nalType === 5) keyframe = true;
      if (nalType === 1 || nalType === 5) break;
    }
    zeros = 0;
  }
  return { keyframe, sps };
}

/**
 * The codec string WebCodecs wants, e.g. "avc1.42E01F", taken from the SPS the
 * encoder emitted rather than assumed: profile and level change with the frame
 * size, and a wrong guess is rejected at configure() time.
 */
function codecFromSps(sps: Uint8Array): string | null {
  if (sps.length < 4) return null;
  const hex = (v: number) => v.toString(16).padStart(2, "0").toUpperCase();
  return `avc1.${hex(sps[1])}${hex(sps[2])}${hex(sps[3])}`;
}

export class VideoStream {
  #ws: WebSocket | null = null;
  #handlers: Handlers;
  #stopped = false;
  #firstFrameTimer: number | null = null;
  #decodingJpeg = false;
  #decoder: VideoDecoder | null = null;
  #decoderCodec: string | null = null;
  /** Nothing before the first keyframe can be decoded. */
  #hadKeyframe = false;
  #lastMeta = { width: 0, height: 0, sequence: 0, pts: 0 };

  constructor(handlers: Handlers) {
    this.#handlers = handlers;
    this.#connect();
  }

  stop() {
    this.#stopped = true;
    if (this.#firstFrameTimer !== null) clearTimeout(this.#firstFrameTimer);
    this.#resetDecoder();
    this.#ws?.close();
    this.#ws = null;
  }

  #connect() {
    if (this.#stopped) return;
    const proto = location.protocol === "https:" ? "wss" : "ws";
    let ws: WebSocket;
    try {
      ws = new WebSocket(`${proto}://${location.host}/video`);
    } catch {
      this.#handlers.onUnavailable("the video channel could not be opened");
      return;
    }
    ws.binaryType = "arraybuffer";
    this.#ws = ws;

    ws.onopen = () => {
      /* Subscribing happens on our first message: the device cannot send
         during the handshake. */
      ws.send(new Uint8Array([1]));
      this.#firstFrameTimer = window.setTimeout(() => {
        this.#handlers.onUnavailable("no frames arrived on the video channel");
      }, FIRST_FRAME_TIMEOUT_MS);
    };

    ws.onmessage = (ev) => void this.#onMessage(ev);

    ws.onclose = () => {
      if (this.#stopped) return;
      this.#handlers.onUnavailable("the video channel closed");
    };
  }

  #resetDecoder() {
    if (this.#decoder) {
      if (this.#decoder.state !== "closed") this.#decoder.close();
      this.#decoder = null;
    }
    this.#decoderCodec = null;
  }

  async #onMessage(ev: MessageEvent) {
    if (!(ev.data instanceof ArrayBuffer) || ev.data.byteLength <= HEADER_LEN) return;
    const view = new DataView(ev.data);
    if (view.getUint8(0) !== MAGIC) return;
    const type = view.getUint8(1);

    if (this.#firstFrameTimer !== null) {
      clearTimeout(this.#firstFrameTimer);
      this.#firstFrameTimer = null;
    }

    this.#lastMeta = {
      width: view.getUint16(2, true),
      height: view.getUint16(4, true),
      sequence: view.getUint32(6, true),
      pts: view.getUint16(10, true),
    };

    if (type === TYPE_JPEG) {
      this.#resetDecoder();
      await this.#onJpeg(ev.data);
    } else if (type === TYPE_H264) {
      this.#onH264(new Uint8Array(ev.data, HEADER_LEN));
    }
  }

  async #onJpeg(buffer: ArrayBuffer) {
    /* Decoding is asynchronous; dropping frames that arrive mid-decode keeps
       latency from growing without bound on a slow client. */
    if (this.#decodingJpeg) return;
    this.#decodingJpeg = true;
    const meta = this.#lastMeta;
    try {
      const blob = new Blob([buffer.slice(HEADER_LEN)], { type: "image/jpeg" });
      const bitmap = await createImageBitmap(blob);
      this.#handlers.onFrame({ image: bitmap, ...meta });
    } catch {
      /* A corrupt frame is not worth reporting; the next one will do. */
    } finally {
      this.#decodingJpeg = false;
    }
  }

  #onH264(payload: Uint8Array) {
    if (typeof VideoDecoder === "undefined") {
      /*
       * Browsers expose WebCodecs in secure contexts only, so a device reached
       * over plain HTTP cannot decode H.264 however capable the browser is.
       * Saying "your browser does not support it" would send the operator
       * looking in the wrong place.
       */
      this.#handlers.onCodecError(
        window.isSecureContext
          ? "this browser has no WebCodecs, so it cannot decode H.264"
          : "H.264 needs a secure page - browsers offer WebCodecs over HTTPS only, and this " +
            "console was loaded over HTTP",
      );
      return;
    }

    const { keyframe, sps } = inspectAnnexB(payload);
    if (sps) {
      const codec = codecFromSps(sps);
      /* A resolution change produces a new SPS and usually a new level, which
         the running decoder was not configured for. */
      if (codec && codec !== this.#decoderCodec) {
        this.#resetDecoder();
        this.#configure(codec);
      }
    }
    if (!this.#decoder) return; /* waiting for a keyframe to configure from */
    if (this.#decoder.state !== "configured") return;

    /* The device holds back delta frames until a viewer has had a keyframe, so
       this should not trigger - but a decoder fed a delta frame it has no
       reference for produces artefacts that look like a hardware fault. */
    if (!keyframe && !this.#hadKeyframe) return;
    if (keyframe) this.#hadKeyframe = true;

    /* Frames queued behind a decoder that cannot keep up are latency, not
       smoothness. */
    if (this.#decoder.decodeQueueSize > 2 && !keyframe) return;

    try {
      this.#decoder.decode(
        new EncodedVideoChunk({
          type: keyframe ? "key" : "delta",
          /* Microseconds, monotonic. The device timestamp wraps every minute,
             which the decoder would read as time running backwards. */
          timestamp: Math.round(performance.now() * 1000),
          data: payload,
        }),
      );
    } catch {
      this.#resetDecoder();
      this.#hadKeyframe = false;
    }
  }

  #configure(codec: string) {
    let decoder: VideoDecoder;
    try {
      decoder = new VideoDecoder({
        output: (frame) => {
          const meta = this.#lastMeta;
          this.#handlers.onFrame({
            image: frame,
            width: frame.displayWidth || meta.width,
            height: frame.displayHeight || meta.height,
            sequence: meta.sequence,
            pts: meta.pts,
          });
        },
        error: (err) => {
          this.#resetDecoder();
          this.#hadKeyframe = false;
          this.#handlers.onCodecError(`the H.264 decoder failed: ${err.message}`);
        },
      });
      decoder.configure({ codec, optimizeForLatency: true });
    } catch (err) {
      this.#handlers.onCodecError(
        `this browser refused the H.264 stream (${codec}): ${(err as Error).message}`,
      );
      return;
    }
    this.#decoder = decoder;
    this.#decoderCodec = codec;
    this.#hadKeyframe = false;
  }
}
