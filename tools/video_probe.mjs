/*
 * Watch the device's video channel without a browser.
 *
 *   node tools/video_probe.mjs espkvm.local 6 [out.h264]
 *
 * Prints frame type and size and, for H.264, the NAL units in each frame -
 * which is how you tell an IDR carrying its parameter sets from a P-frame.
 * Given a third argument it also writes the raw Annex-B stream, so the picture
 * can be checked against a decoder that is not a browser:
 *
 *   ffmpeg -i out.h264 -frames:v 1 frame.png
 *
 * That check is worth doing after anything touches the colour conversion: a
 * wrong stride or a missed crop still decodes, it just looks wrong.
 */
import { createRequire } from "node:module";
import { createWriteStream } from "node:fs";

/* `ws` belongs to the console's dependencies; resolve it from there rather
 * than asking the repository root for a node_modules of its own. */
const require = createRequire(new URL("../web/package.json", import.meta.url));
const WebSocket = require("ws");

const host = process.argv[2] ?? "espkvm.local";
const seconds = Number(process.argv[3] ?? 5);
const dumpPath = process.argv[4];

const dump = dumpPath ? createWriteStream(dumpPath) : null;
/* The device's certificate is self-signed, which is the point of it. */
const ws = new WebSocket(`wss://${host}/video`, { rejectUnauthorized: false });

let frames = 0;
let bytes = 0;
let keyframes = 0;
let started = 0;

/** NAL unit types in an Annex-B frame: 7 SPS, 8 PPS, 5 IDR, 1 inter-coded. */
function nals(buf) {
  const out = [];
  let zeros = 0;
  for (let i = 0; i < buf.length && out.length < 12; i++) {
    const b = buf[i];
    if (b === 0) {
      zeros++;
      continue;
    }
    if (b === 1 && zeros >= 2) out.push(buf[i + 1] & 0x1f);
    zeros = 0;
  }
  return out;
}

ws.on("open", () => {
  /* The device subscribes a viewer on its first message, not at the
     handshake - it cannot send during the upgrade. */
  ws.send(Buffer.from([1]));
  started = Date.now();
  setTimeout(() => {
    const secs = (Date.now() - started) / 1000;
    console.log(
      `\n${frames} frames in ${secs.toFixed(1)}s = ${(frames / secs).toFixed(1)} fps, ` +
        `${((bytes * 8) / secs / 1000).toFixed(0)} kbit/s, ${keyframes} keyframes`,
    );
    if (dump) {
      dump.end();
      console.log(`wrote ${dumpPath}`);
    }
    ws.close();
    process.exit(0);
  }, seconds * 1000);
});

ws.on("message", (data) => {
  const buf = Buffer.from(data);
  const type = buf[1];
  const w = buf.readUInt16LE(2);
  const h = buf.readUInt16LE(4);
  const seq = buf.readUInt32LE(6);
  const payload = buf.subarray(12);
  frames++;
  bytes += payload.length;

  let detail = "";
  if (type === 2) {
    const list = nals(payload);
    if (list.includes(5)) keyframes++;
    detail = ` nal=[${list.join(",")}]`;
    if (dump) dump.write(payload);
  }
  if (frames <= 4 || frames % 25 === 0) {
    console.log(
      `#${frames} seq=${seq} ${type === 2 ? "h264" : "jpeg"} ${w}x${h} ${payload.length}B${detail}`,
    );
  }
});

ws.on("error", (e) => {
  console.error("ws error:", e.message);
  process.exit(1);
});
