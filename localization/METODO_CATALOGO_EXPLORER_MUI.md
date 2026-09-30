# Method for the explorer.exe.mui localization catalog

> **v0.0.3 (2026-09-28)**: the catalog is no longer used to *fill* resources in a reference `.mui` file (that transplant was removed). The text described here is now emitted as **PE resources generated entirely by this project**: structure from `localization/templates/`, text from `localization/catalog/`, and generation/validation by `tools/build_resources.py`. The copyright rule below is unchanged and is even more important: Microsoft text must NOT be added to the repository.

*(Instructions agreed with the project owner on 2026-09-28; procedure tested with dialog 205.)*

## Golden rule (copyright)

Do NOT copy or translate Microsoft text from the `.mui` file word for word. Do not put original Microsoft text in `localization/catalog/*.json` (the repository is public and GPLv3, which permits redistribution).

**Permitted method**: read the original string's meaning/function locally (never paste it into chat or a commit), then write your OWN sentence—same UI function, different wording, and no imitation of the original sentence structure.

## Step-by-step procedure

1. Start with `localization/constraints/explorer.exe.constraints.json` (structure only, no text: 161 strings, 6 menus, 6 dialogs, and 1 accelerator table).
2. For each ID, read `len` (guideline length), `accel` (accelerator letter to preserve as a CONCEPT—one `&` per string, unique among controls/items in the same dialog or menu), and `placeholders` (e.g. `%s`, `%1!s!`; preserve the same number/order/type, NEVER change them).
3. Write an original sentence in the target language that conveys the same function. Length is flexible but must stay within the budget: `max(2 * len_original, len_original + 8)` characters (excluding `&`).
4. ALWAYS validate before submitting: no duplicate `&` markers in one string, identical placeholders by number/order/type, no duplicate accelerators within the same context (same dialog or menu level), and length within budget.
5. Work in SMALL BATCHES (one dialog or menu at a time) and show the result with its validation table before moving to the next batch, so the project owner can review as you go.

## Validation scripts

The helpers are in `tools/analyze_mui.py` (actual API names in this repository): `extract_accel` (raises `ValueError` if there is more than one `&`), `extract_placeholders` (raises `ValueError` for a stray `%`), and `strip_accels` (note the plural). The definitive checker that must pass in CI is `tools/verify_catalog.py`.

## Completed and validated example: dialog 205 ("Start Menu" tab)

| Control | Text (Italian sample) |
|---|---|
| Title | Menu &Start |
| 1134 (static) | Per decidere come appaiono e si comportano collegamenti, icone e menu nel menu Start, premi Personalizza. |
| 1131 (button) | Personali&zza... |
| 65535 (static) | Azione del p&ulsante di alimentazione: |
| 1136 (checkbox) | Mostra i &programmi aperti di recente nel menu Start |
| 1135 (checkbox) | Mostra gli elementi aperti di recente nel &menu Start e nella barra delle applicazioni |
| 300 (button) | Privacy |
| 1116 (link) | Come faccio a cambiare l'aspetto del menu Start? |

Validation result: 0 errors; the dialog uses accelerators `Z U P M` (no duplicates); every string is within budget.

## Still to do

- Dialogs: 6, 10, 20, 23, 1036 (205 is done)
- Menus: 205, 211, 212, 213, 6003, 12000 (none done)
- Standalone strings: 161 IDs in `constraints → "strings"`, many already associated with a control/item; process the rest in batches of about 15–20.

## Extending to other languages

Repeat the same procedure (read the meaning → write an original sentence → validate) for each target language and save it to `localization/catalog/<lang>.json` using the schema in `localization/schema.md`. English (`en.json`) is the mandatory fallback and must cover EVERY ID before other languages are added.

Native-speaker review priority: Japanese and Chinese (accelerator choices for ideographic writing), then Russian, followed by the Latin-script languages.

## Catalog format for dialogs/menus (implemented in verify_catalog.py, 2026-09-28)

```json
"dialogs": {
  "6/lang:0409": {
    "title": "...",
    "controls": { "1105": "...", "65535#1": "...", "65535#2": "..." }
  }
},
"menus": { "211/lang:0409": { "1/0": "...", "1/1": "..." } }
```

- Controls with **duplicate IDs** (e.g. 65535, 4294967295): use `<id>#N` keys in template order (N starts at 1); for a unique ID, use a simple key such as `"1131"`.
- Menu items: use the key `"level/index"` (zero-based) from the constraints structure; items without text (separators) are not translated.
- Length overrides: use keys such as `"dialog:<dlg>:<ctrl>"`, `"dialog:<dlg>:title"`, and `"menu:<menu>:<l/i>"` inside `maxlen_override`; a comment is required.
- Accelerators must be unique within each dialog (excluding its title) and each menu level. The checker warns if a control marked as having an accelerator in the reference has no `&` (or vice versa).
- `require_fallback_coverage` in the constraints file: only sources set to `true` require full coverage in `en.json` (shell32 = true; `explorer.exe.mui` remains false until `en` covers all 161 IDs).

## Standard terminology: keep terms recognizable

The project owner's decision (2026-09-28): for standard terms (such as the Italian **"Barra delle applicazioni"** [taskbar], **"Menu Start"**, and **"Area di notifica"** [notification area]), there is NO need to make every sentence different for its own sake—recognizable terminology takes priority. Creative rewording is mainly useful for long, descriptive strings. If a string exceeds the budget, use `maxlen_override` with a comment rather than distorting the sentence.

## Confirming behavior against the real file (locally; never commit the text)

Extract the original text only on a machine that has the file:

```
python tools/analyze_mui.py explorer.exe.mui --with-strings > mui-full.json
```

`--with-strings` includes ORIGINAL text (strings, menu items, dialog controls). Use it to UNDERSTAND the function, then write original wording. Do not commit the output or paste it into chat/PR. To check drafts, compare the original dialog on screen with the draft table.
