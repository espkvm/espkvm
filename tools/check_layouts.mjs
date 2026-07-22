/*
 * Validate the paste layout tables in web/src/layouts.js.
 *
 *     node tools/check_layouts.mjs
 *
 * A duplicated key position is the failure mode worth catching here: two
 * characters mapped to the same (modifier, scancode) means pasting text
 * silently types the wrong one, and nothing reports an error. Checking by hand
 * across 33 Cyrillic letters is exactly the kind of review that misses one.
 */

import { LAYOUTS } from "../web/src/layouts.js";

const RU_ALPHABET = "абвгдеёжзийклмнопрстуфхцчшщъыьэюя";
const EN_ALPHABET = "abcdefghijklmnopqrstuvwxyz";

let failures = 0;

function fail(message) {
  console.log(`  FAIL ${message}`);
  failures++;
}

/* Deliberate aliases: both line endings press Enter, which is what a target
 * expects regardless of the convention the pasted text was written with. */
const ALLOWED_ALIASES = [new Set(["\r", "\n"])];

function isAlias(a, b) {
  return ALLOWED_ALIASES.some((group) => group.has(a) && group.has(b));
}

function checkDuplicates(id, map) {
  const seen = new Map();
  for (const [ch, entry] of Object.entries(map)) {
    const slot = `${entry.mod}:${entry.hid}`;
    if (seen.has(slot) && isAlias(ch, seen.get(slot))) {
      continue;
    }
    if (seen.has(slot)) {
      fail(`${id}: ${JSON.stringify(ch)} and ${JSON.stringify(seen.get(slot))} ` +
           `both use modifier ${entry.mod} scancode ${entry.hid}`);
    } else {
      seen.set(slot, ch);
    }
  }
}

function checkAlphabet(id, map, alphabet) {
  for (const ch of alphabet) {
    if (!map[ch]) {
      fail(`${id}: lowercase ${JSON.stringify(ch)} is missing`);
      continue;
    }
    const upper = ch.toUpperCase();
    if (!map[upper]) {
      fail(`${id}: uppercase ${JSON.stringify(upper)} is missing`);
      continue;
    }
    if (map[upper].hid !== map[ch].hid) {
      fail(`${id}: ${JSON.stringify(upper)} uses a different key than ${JSON.stringify(ch)}`);
    }
    if (map[upper].mod === map[ch].mod) {
      fail(`${id}: ${JSON.stringify(upper)} carries the same modifiers as ${JSON.stringify(ch)}`);
    }
  }
}

function checkRanges(id, map) {
  for (const [ch, entry] of Object.entries(map)) {
    if (!Number.isInteger(entry.hid) || entry.hid < 0x04 || entry.hid > 0xe7) {
      fail(`${id}: ${JSON.stringify(ch)} has scancode ${entry.hid}, outside the usage range`);
    }
    if (!Number.isInteger(entry.mod) || entry.mod < 0 || entry.mod > 0xff) {
      fail(`${id}: ${JSON.stringify(ch)} has modifier ${entry.mod}`);
    }
  }
}

for (const [id, layout] of Object.entries(LAYOUTS)) {
  const count = Object.keys(layout.map).length;
  console.log(`${id} (${layout.label}): ${count} characters`);
  checkDuplicates(id, layout.map);
  checkRanges(id, layout.map);
  if (id === "en_us") checkAlphabet(id, layout.map, EN_ALPHABET);
  if (id === "ru_ru") checkAlphabet(id, layout.map, RU_ALPHABET);
  if (!failures) console.log("  ok");
}

if (failures) {
  console.log(`\n${failures} problem(s)`);
  process.exit(1);
}
console.log("\nall layouts consistent");
