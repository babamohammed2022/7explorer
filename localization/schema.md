# Schema dei dati di localizzazione

```
localization/
├── catalog/<lang>.json              # testi ORIGINALI, una cartella per lingua
└── constraints/<modulo>.constraints.json
                                     # struttura di riferimento (no testo)
```

## constraints/*.constraints.json

Generato da `tools/analyze_mui.py --constraints ...` (o scritto a mano quando
la struttura è dimostrata dal sorgente, come per `shell32.dll`).

```json
{
  "source": "da dove viene la struttura (obbligatorio)",
  "source_sha256": "hash del file di riferimento, se generato da analyze_mui",
  "strings": {
    "<id>": { "len": <int>, "accel": "<lettera|null>", "placeholders": ["%1!s!"] }
  },
  "contexts": {
    "<nome>": { "members": ["<id>"], "coex": <bool>, "why": "nota" }
  },
  "menus":   { "<id>/lang:0409": { "levels": [[{"cmd": 101, "accel": "F", ...}]] } },
  "dialogs": { "<id>/lang:0409": { "rect": [x,y,cx,cy], "controls": [...] } }
}
```

`coex: false` = i membri non sono mai visibili insieme (es. pin/unpin);
altrimenti `verify_catalog.py` esige acceleratori distinti nel contesto.

## catalog/<lang>.json

```json
{
  "language": {
    "id": "it", "mui_name": "it-IT", "lcid": "0x0410",
    "name": "italiano", "needs_native_review": false
  },
  "strings": { "5381": "Aggiungi al menu &Start" },
  "maxlen_override": { "5384": 64, "dialog:6/lang:0409:title": 40 },
  "maxlen_override_comment": { "5384": "perché serve (obbligatorio)" },
  "dialogs": { "6/lang:0409": {
    "title": "Barra delle applicazioni",
    "controls": { "1105": "...", "65535#1": "..." } } },
  "menus": { "211/lang:0409": { "1/0": "..." } }
}
```

Regole (tutte applicate da `tools/verify_catalog.py`):

1. inglese (`en.json`) presente e completo = fallback per ogni fonte
   constraints con `\"require_fallback_coverage\": true`;
2. nessun ID che non esista nei constraints ("orphan" = errore);
3. segnaposto identici per numero, ordine e tipo;
4. al massimo un acceleratore `&` per stringa; unicità nei contesti coex;
5. lunghezza ≤ max(2×riferimento, riferimento+8) salvo override commentato;
6. il testo è ORIGINALE: non copiare/tradurre letteralmente il testo Microsoft
   (riformula il significato con parole tue).

Controlli con id duplicato nel dialog: chiavi `<id>#N` in ordine di
template. Voci di menu: chiavi `<livello>/<indice>`. Dettagli operativi in
`localization/METODO_CATALOGO_EXPLORER_MUI.md`.

`tools/embed_catalog.py` genera da qui `installer/Win7ExplorerRestorer/lang_catalog.h`
e `explorerwrapper/Win7ExplorerRestorer_languages.rc` e rifiuta di emettere se la verifica fallisce.
