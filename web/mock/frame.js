/*
 * Generates the picture the mock device "captures".
 *
 * The video pane has to be exercised with something that actually changes,
 * otherwise scaling, letterboxing and the pointer mapping cannot be judged.
 * PNG is written by hand here rather than pulling in an image library: an
 * uncompressed PNG is a header, one IDAT of deflated scanlines and a CRC.
 */

import zlib from "node:zlib";

const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = -1;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

/** Draw a 5x7 block digit, enough to read a frame counter on screen. */
const GLYPHS = {
  0: ["111", "101", "101", "101", "111"],
  1: ["010", "110", "010", "010", "111"],
  2: ["111", "001", "111", "100", "111"],
  3: ["111", "001", "111", "001", "111"],
  4: ["101", "101", "111", "001", "001"],
  5: ["111", "100", "111", "001", "111"],
  6: ["111", "100", "111", "101", "111"],
  7: ["111", "001", "010", "010", "010"],
  8: ["111", "101", "111", "101", "111"],
  9: ["111", "101", "111", "001", "111"],
  ":": ["000", "010", "000", "010", "000"],
  x: ["000", "101", "010", "101", "000"],
};

export function renderFrame(width, height, tick) {
  const rgb = Buffer.alloc(width * height * 3);

  /* A gradient background so scaling artefacts and colour handling show up. */
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 3;
      rgb[i] = 30 + Math.floor((x / width) * 60);
      rgb[i + 1] = 34 + Math.floor((y / height) * 40);
      rgb[i + 2] = 60 + Math.floor((x / width) * 90);
    }
  }

  /* A grid, to make the edges of the pane obvious. */
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      if (x % 160 === 0 || y % 160 === 0) {
        const i = (y * width + x) * 3;
        rgb[i] = 70;
        rgb[i + 1] = 78;
        rgb[i + 2] = 110;
      }
    }
  }

  /* Something moving, so a frozen stream is visible at a glance. */
  const bx = Math.floor((Math.sin(tick / 12) * 0.35 + 0.5) * (width - 160));
  const by = Math.floor((Math.cos(tick / 17) * 0.3 + 0.5) * (height - 160));
  for (let y = by; y < by + 160 && y < height; y++) {
    for (let x = bx; x < bx + 160 && x < width; x++) {
      const i = (y * width + x) * 3;
      rgb[i] = 240;
      rgb[i + 1] = 150;
      rgb[i + 2] = 70;
    }
  }

  const label = `${width}x${height}`;
  drawText(rgb, width, height, label, 40, 40, 6);
  drawText(rgb, width, height, String(tick % 1000), 40, 40 + 7 * 6 + 20, 6);

  /* PNG scanlines are prefixed with a filter byte. */
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (width * 3 + 1)] = 0;
    rgb.copy(raw, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
  }

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // colour type: truecolour
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(raw, { level: 1 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

function drawText(rgb, width, height, text, ox, oy, scale) {
  for (let c = 0; c < text.length; c++) {
    const glyph = GLYPHS[text[c]];
    if (!glyph) continue;
    for (let gy = 0; gy < glyph.length; gy++) {
      for (let gx = 0; gx < glyph[gy].length; gx++) {
        if (glyph[gy][gx] !== "1") continue;
        for (let sy = 0; sy < scale; sy++) {
          for (let sx = 0; sx < scale; sx++) {
            const x = ox + c * 4 * scale + gx * scale + sx;
            const y = oy + gy * scale + sy;
            if (x >= width || y >= height) continue;
            const i = (y * width + x) * 3;
            rgb[i] = 235;
            rgb[i + 1] = 240;
            rgb[i + 2] = 250;
          }
        }
      }
    }
  }
}
