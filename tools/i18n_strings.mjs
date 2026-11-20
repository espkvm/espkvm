/*
 * The console's translatable text, and how far each language covers it.
 *
 *     node tools/i18n_strings.mjs               coverage of every web/locales/*.json
 *     node tools/i18n_strings.mjs --missing ru  what ru lacks, as JSON to fill in
 *     node tools/i18n_strings.mjs --check       fail on a broken call or language file
 *     node tools/i18n_strings.mjs --publish DIR the files fw.espkvm.io/locales/ serves
 *
 * The English text is the key (see web/src/i18n.ts). It is collected from:
 * - t("..."), tn("...", "...", n) and tm("...") calls in web/src, string literals only;
 * - the settings the firmware describes (components/kvm_config/kvm_settings_table.c):
 *   titles, help, group names and choice labels, which the console passes
 *   through t() as they arrive.
 */
import { copyFileSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
const src = join(root, "web", "src");
const localesDir = join(root, "web", "locales");

function walk(dir, out = []) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      if (name !== "demo") walk(p, out);
    } else if (/\.(ts|vue)$/.test(name) && name !== "layoutsGen.ts") {
      out.push(p);
    }
  }
  return out;
}

/* A JS string literal starting at s[i] (a quote), decoded. */
function literal(s, i) {
  const q = s[i];
  let out = "";
  for (let j = i + 1; j < s.length; j++) {
    const c = s[j];
    if (c === "\\") {
      const n = s[++j];
      out += n === "n" ? "\n" : n === "t" ? "\t" : n === "u" ? String.fromCharCode(parseInt(s.slice(j + 1, (j += 4) + 1), 16)) : n;
    } else if (c === q) {
      return { text: out, end: j + 1 };
    } else if (q === "`" && c === "$" && s[j + 1] === "{") {
      return null; // a template with ${}: not a fixed key
    } else {
      out += c;
    }
  }
  return null;
}

/* Literal strings joined by +, as long lines get written. */
function joined(s, i) {
  let text = "";
  for (;;) {
    while (/\s/.test(s[i])) i++;
    if (!`"'\``.includes(s[i])) return null;
    const lit = literal(s, i);
    if (!lit) return null;
    text += lit.text;
    i = lit.end;
    let k = i;
    while (/\s/.test(s[k])) k++;
    if (s[k] !== "+") return { text, end: i };
    i = k + 1;
  }
}

const strings = new Map(); // text -> where
const plurals = new Map(); // the English singular of each tn() -> its English plural
const problems = [];

function add(text, where) {
  if (text.trim() && !strings.has(text)) strings.set(text, where);
}

for (const file of walk(src)) {
  const s = readFileSync(file, "utf8");
  const rel = relative(root, file);
  const re = /(?<![\w.$])(t|tn|tm)\(/g;
  let m;
  while ((m = re.exec(s))) {
    const line = s.slice(0, m.index).split("\n").length;
    const first = joined(s, m.index + m[0].length);
    if (!first) {
      /* t(setting.title) and friends: firmware text, collected below. A
         template with ${} is a mistake: its key changes with the value. */
      let k = m.index + m[0].length;
      while (/\s/.test(s[k])) k++;
      if (s[k] === "`") problems.push(`${rel}:${line}: ${m[1]}() with a \${} template - use {placeholders}`);
      continue;
    }
    add(first.text, `${rel}:${line}`);
    if (m[1] === "tn") {
      let k = first.end;
      while (/[\s,]/.test(s[k])) k++;
      const second = joined(s, k);
      plurals.set(first.text, second ? second.text : first.text);
    }
  }
}

/* The firmware's setting text. */
{
  const c = readFileSync(join(root, "components", "kvm_config", "kvm_settings_table.c"), "utf8");
  const cstr = (body) => [...body.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1].replace(/\\"/g, '"').replace(/\\\\/g, "\\")).join("");
  for (const m of c.matchAll(/\.(title|help|group|unit)\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+)/g)) {
    add(cstr(m[2]), "kvm_settings_table.c");
  }
  for (const m of c.matchAll(/static const char \*const s_\w+_labels\[\]\s*=\s*\{([^}]*)\}/g)) {
    for (const x of m[1].matchAll(/"((?:[^"\\]|\\.)*)"/g)) add(x[1], "kvm_settings_table.c");
  }
  /* The sections: id, title, blurb. */
  const table = c.match(/s_sections\[\]\s*=\s*\{([\s\S]*?)\n\};/);
  for (const row of table ? table[1].matchAll(/\{\s*"[^"]*",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"/g) : []) {
    add(row[1], "kvm_settings_table.c");
    add(row[2], "kvm_settings_table.c");
  }
  /* A setting with no labels shows its choices as they are ("Full", "arrow"):
     those, from the entries that name .choices and no .labels. */
  const arrays = new Map();
  for (const m of c.matchAll(/static const char \*const (s_\w+)\[\]\s*=\s*\{([^}]*)\}/g)) {
    arrays.set(m[1], [...m[2].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]));
  }
  for (const entry of c.split(/\n\s*\{\s*\n\s*(?:\/\*[\s\S]*?\*\/\s*)?\.key\s*=/).slice(1)) {
    const body = entry.split(/\n    \},/)[0];
    const ch = body.match(/\.choices\s*=\s*(s_\w+)/);
    if (!ch || /\.labels\s*=/.test(body)) continue;
    /* Words only: baud rates, part numbers and modes like 1080p30 stay as they are. */
    /* Capitalised the way choiceLabel() in state/device.ts shows them. */
    for (const x of arrays.get(ch[1]) ?? []) {
      if (/[a-z]{3,}/.test(x)) add(x.charAt(0).toUpperCase() + x.slice(1), "kvm_settings_table.c");
    }
  }
}

const placeholders = (s) => [...String(s).matchAll(/\{(\w+)\}/g)].map((x) => x[1]).sort().join(",");

function loadLocales() {
  let names = [];
  try {
    names = readdirSync(localesDir).filter((n) => /^[a-z]{2,3}(-[A-Za-z0-9]+)?\.json$/.test(n));
  } catch {}
  return names.map((n) => {
    const text = readFileSync(join(localesDir, n), "utf8");
    let data;
    try {
      data = JSON.parse(text);
    } catch (e) {
      problems.push(`web/locales/${n}: not JSON (${e.message})`);
      return null;
    }
    if (data["@language"] !== n.replace(/\.json$/, "")) {
      problems.push(`web/locales/${n}: "@language" must be ${n.replace(/\.json$/, "")}`);
    }
    const tr = data["@translators"];
    if (tr !== undefined && (!Array.isArray(tr) || tr.some((x) => typeof x !== "string" || !x.trim() || x.length > 80))) {
      problems.push(`web/locales/${n}: "@translators" must be a list of names, each up to 80 characters`);
    }
    for (const [k, v] of Object.entries(data)) {
      if (k.startsWith("@")) continue;
      const forms = typeof v === "string" ? [v] : Object.values(v ?? {});
      for (const f of forms) {
        if (typeof f !== "string") problems.push(`web/locales/${n}: ${JSON.stringify(k)} is not text`);
        else {
          /* A translation may only use the English placeholders, and {n} in
             a count. Anything else would show up unfilled. */
          const allowed = new Set([...placeholders(k).split(","), "n"]);
          const extra = placeholders(f).split(",").filter((p) => p && !allowed.has(p));
          if (extra.length) problems.push(`web/locales/${n}: ${JSON.stringify(k)} uses {${extra.join("}, {")}}, which the English does not have`);
        }
      }
    }
    return { code: n.replace(/\.json$/, ""), data };
  }).filter(Boolean);
}

const locales = loadLocales();
const args = process.argv.slice(2);

/* What a translation fills in: every string, a count once under its English
   singular (its English plural is not a key of its own). */
const entries = [...strings.keys()];
const translated = (data) => entries.filter((k) => k in data).length;
const percent = (data) => Math.floor((100 * translated(data)) / entries.length);

if (args[0] === "--missing") {
  const loc = locales.find((l) => l.code === args[1]);
  const out = { "@language": args[1] };
  /* A count gets one entry per plural form of the language. */
  const forms = new Intl.PluralRules(args[1]).resolvedOptions().pluralCategories;
  for (const k of entries) {
    if (loc && k in loc.data) continue;
    out[k] = plurals.has(k) ? Object.fromEntries(forms.map((f) => [f, ""])) : "";
  }
  process.stdout.write(JSON.stringify(out, null, 2) + "\n");
  process.exit(0);
}

/*
 * What fw.espkvm.io/locales/ serves: the language files, index.json (the
 * list the console offers, with how much of each is translated) and
 * strings.json (every English string, for the console's translation editor).
 */
if (args[0] === "--publish") {
  const dir = args[1];
  mkdirSync(dir, { recursive: true });
  const index = [];
  for (const l of locales) {
    copyFileSync(join(localesDir, `${l.code}.json`), join(dir, `${l.code}.json`));
    index.push({ code: l.code, name: l.data["@name"] ?? l.code, offer: l.data["@offer"], translators: l.data["@translators"], translated: translated(l.data), total: entries.length, percent: percent(l.data) });
  }
  writeFileSync(join(dir, "index.json"), JSON.stringify(index) + "\n");
  const list = entries.map((en) => (plurals.has(en) ? { en, other: plurals.get(en) } : { en }));
  writeFileSync(join(dir, "strings.json"), JSON.stringify({ strings: list }) + "\n");
  console.log(`published ${index.length} language(s) and ${entries.length} strings to ${dir}`);
  process.exit(problems.length ? 1 : 0);
}

console.log(`${entries.length} strings to translate`);
for (const l of locales) {
  const keys = Object.keys(l.data).filter((k) => !k.startsWith("@"));
  const done = translated(l.data);
  console.log(`  ${l.code}: ${done} translated (${percent(l.data)}%), ${keys.length - done} no longer used`);
}
if (problems.length) {
  for (const p of problems) console.log(`  FAIL ${p}`);
  if (args.includes("--check")) process.exit(1);
}
