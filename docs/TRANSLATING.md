# Translating the console

The console is written in English. Other languages are JSON files that the
console downloads into the browser: Settings -> UI -> Language. Each viewer
can pick their own. A device with no internet can be given the file by hand
with "From a file...".

The device can also keep one language of its own (Settings -> UI -> Device
language). Every browser without a choice of its own then opens in it,
sign-in page included, with no internet needed. It sits in the flash
`storage` partition.

The console checks a language file every time it loads one, from the site,
from a file or from the device. A file that is not a language file at all
(not JSON, no `@language`, over 512 KB) is refused. A single bad entry (not
text, a placeholder the English lacks, a control character) is left out and
shows in English, and the console says how many it left out. Translations are
only ever shown as text, never as markup.

The files live in the console's repository, in `web/locales/<code>.json`. Each
release publishes them at `https://fw.espkvm.io/locales/`, with an `index.json`
listing them.

## The file

```json
{
  "@language": "lt",
  "@name": "Lietuvių",
  "@offer": "Konsolė yra ir lietuvių kalba: Nustatymai → Sąsaja → Kalba.",
  "@translators": ["Rūta", "Jonas"],
  "Paste from clipboard": "Įklijuoti iš iškarpinės",
  "Stopped after {n} of {total} characters": "Sustabdyta po {n} iš {total} simbolių",
  "Copied {n} line from the screen": {
    "one": "Nukopijuota {n} eilutė iš ekrano",
    "few": "Nukopijuotos {n} eilutės iš ekrano",
    "many": "Nukopijuota {n} eilutės iš ekrano",
    "other": "Nukopijuota {n} eilučių iš ekrano"
  }
}
```

- The key is the English text itself. When I change a sentence in English,
  its old translation stops matching and the console shows the new English
  until the translation catches up. A stale translation never shows.
- `{name}` placeholders are filled in by the console. Keep each one, and use
  no others (`{n}` is always allowed in a count).
- A count is an object with the language's plural forms (`one`, `few`, `many`,
  `other` and so on, as `Intl.PluralRules` names them), under the English
  singular.
- `@translators` is the list of people who translated it, as they want to be
  named. The console shows it under the language choice, and the
  translations page next to the language. Add your name when you send a file.
- `@name` is the language's name in itself. `@offer` is one line, in the
  language, that the console shows once to a browser set to that language.
- Text the device sends (setting titles and help, error messages) goes through
  the same file, so it is translated the same way.

## In the console

Settings -> UI -> Language -> "Edit..." opens an editor: every English string
next to a field for its translation, how much is done, a filter for what is
missing or wrong, and search in both languages. A change shows on the console
at once. "Export" saves the file, "Keep on the device" gives it to the device.

It works in the demo too, with no device at all: https://demo.espkvm.io/.
https://espkvm.io/translations/ lists the languages with how much of each is
done, shows each one next to the English, and links into the demo:
`?lang=de&edit=1` opens the console in German with the editor open,
`?edit=new` starts an empty language. The same works on a device. The
list of strings comes from `https://fw.espkvm.io/locales/strings.json`, which
can be downloaded and loaded by hand where that site cannot be reached.

## Taking a language

Before you start, open an issue at https://github.com/espkvm/espkvm/issues
saying which language you are translating, so two people do not do the same
one. Check the open issues first: someone may already be on it.

When it is done, or done enough to use, send the file as a pull request to
`web/locales/<code>.json` in https://github.com/espkvm/console, or attach it
to the issue. A partial file is fine.

## What is left to translate

```sh
node tools/i18n_strings.mjs               # how much each language covers
node tools/i18n_strings.mjs --missing lt  # what lt lacks, ready to fill in
node tools/i18n_strings.mjs --check       # what CI runs
```

`--missing` prints the missing keys with empty values, counts already split
into the language's plural forms. Fill them in and merge them into the file.
Untranslated text simply stays English, so a partial file is fine.

When an English sentence changes, keep the old key and its translation next to
the new one. fw.espkvm.io serves one file to every version, and a device on an
older release still looks the text up by the old English. Old keys are cleared
now and then:

```sh
node tools/i18n_strings.mjs --prune      # drop keys no console of the last 10 releases uses
node tools/i18n_strings.mjs --prune 20   # or of the last 20
node tools/i18n_strings.mjs --history    # which English text left the console in which release
```

CI warns when a language falls behind the English, and when a file has more
than a hundred keys the console no longer uses.

## In the code

`t("English")` where text is shown, `tn("{n} file", "{n} files", n)` for a
count, and `tm("English")` to mark text kept in a table and shown through
`t()` later. Keys are fixed strings: never `t(\`...${x}\`)`, use a placeholder.
`web/src/i18n.ts` has the details.
