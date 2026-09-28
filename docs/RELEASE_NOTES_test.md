# Pre-release di PROVA — ex7 self-contained bootstrap (v0.0.3)

Binari compilati dal CI (GitHub Actions, `windows-latest`, MSVC) nel workflow
`selfcontained-ci`. Hash in `SHA256SUMS.txt`.

## Cosa c'è qui

| File | Cosa è |
| --- | --- |
| `wrp64.dll` | il wrapper explorer7 compilato in Release x64 |
| `ex7selfcontained.exe` | l'installer/bootstrap self-contained |
| `SHA256SUMS.txt` | SHA-256 degli eseguibili sopra |

## Novità v0.0.3 — localizzazione SENZA `.mui` (rottura con v0.0.2)

Dopo i test reali di v0.0.2-test1 (fallimento identico in 3 corrid su
`UpdateResource`), il meccanismo del **trapianto da `explorer.exe.mui`
fornito dall'utente è stato RIMOSSO COMPLETAMENTE**:

- niente `.mui` da procurarsi, niente `EX7_REFERENCE_MUI`, niente identity
  check sul `.mui`, niente valutazione symbol server per `.mui`;
- **tutte** le risorse UI della copia privata (STRINGTABLE, MENU, DIALOGEX,
  ACCELERATOR) sono **generate dal progetto**:
  `localization/catalog/{en,it}.json` (testi del progetto) +
  `localization/templates/explorer.exe.templates.json` (solo struttura) →
  `tools/build_resources.py` (payload PE + rivalidazione round-trip) →
  blob embeddati in `lang_catalog.h`;
- a install time l'installer fa la **riscrittura atomica della tabella
  risorse**: enumera e ricopia tutte le risorse esistenti, omette `MUI`
  (parcheggiato come `CUI`), scrive i blob en-US + it-IT, committa, quindi
  ricalcola il CheckSum PE e registra l'hash FINALE in `state\install.json`;
- **stop netto**: qualsiasi errore di generazione/validazione/iniezione
  interrompe l'installazione (exit 1) con la transazione SCARTATA — mai una
  shell mezzo localizzata. Rimosso `--allow-partial-localization`
  (v0.0.2 lo rendeva cosmetico: il gate era *dopo* il punto di fallimento).

### Root cause v0.0.2 (provata nel CI, per onestà documentale)

Il file reale è un binario **LN marcato MU**; `UpdateResourceW` rifiuta
l'inserimento di risorse con `ERROR_NOT_SUPPORTED (50)` (qualunque lingua),
la cancellazione del solo marcatore `MUI` fallisce con
`ERROR_INVALID_PARAMETER (87)`, mentre `BeginUpdateResource(
bDeleteExistingResources=TRUE)` con riscrittura completa viene accettata.
Probe: `ci/updres/Program.cs` (run 36415198307+); repro originale
`ci-logs/diagloc-36412428950` (`GetLastError=50` su blocco STRING 337).

### Bug corretto lungo la strada

`parse_string_table` assegnava gli ID di stringa con off-by-one
(slot `i` → `base+i` invece di `base+i+1`). Tutti i 161 ID stringa di
`explorer.exe.constraints.json` (derivati dal tool buggy) e i testi catalogo
corrispondenti sono slittati **+1** ai veri ID Win32. I vincoli shell32
(5381/5382/5384/5385, da `StartMenuPin.cpp`) erano e restano corretti.

## Cosa FUNZIONA (verificato nel CI su file reale, ad ogni push/tag)

- Download verificato di `explorer.exe` (hash allow-list, identità PE,
  deadline/cancellazione), riuso offline della copia `.pris`.
- Patch import → `wrp64.dll`: deterministica, idempotente, C++ ≡ Python
  byte-per-byte (`fc /b` nel CI).
- Pipeline risorse: 43 test Python ovunque + sul runner Windows: **run
  end-to-end dell'installer** e prova sul PE prodotto con
  `tools/check_pe_resources.py` (presenza **byte-per-byte** di tutti gli
  88 payload per (tipo,id,lcid), parsing menu/dialog/stringhe/acceleratori,
  `MUI` assente, `CUI` presente) + **secondo run idempotente**.
- Stringhe wrapper (pin/unpin Start menu) in 10 lingue via fallback
  `LoadStringW` (file `.rc` multilingua generato).

## Cosa NON fa ancora

- **Nessuna integrazione shell** (switch di userinit/Winlogon): questa
  release prepara solo la cartella `explorer7/` completa e verificata.
- Aspetti che richiedono **Windows reale eseguito dall'utente**: avvio
  effettivo della shell patchata (rendering taskbar/start menu, dialoghi
  non tronchi a video). Il CI prova tutto ciò che è verificabile statico/
  programmatico, ma non *lancia* la shell.


## Update 2026-09-28 (v0.0.3-test2)

- **Fix portabilità binari**: CRT statico (`/MT`) per `ex7selfcontained.exe`
  e `wrp64.dll` — la v0.0.3-test1 su una VM pulita Win11 24H2 falliva con
  «MSVCP140.dll was not found». Ora nessuna dipendenza dal Visual C++
  Redistributable.
- **Conferma utente (hardware reale Win10 19044)**: bootstrap completo OK,
  3 run idempotenti, hash finale stabile `92291e61…`. Pipeline v0.0.3
  validata su macchina reale.
- **PoC shell via Windhawk** (nuovo, cartella `windhawk/`, anche in asset):
  - `ex7-userinit-shell.wh.cpp` (`userinit.exe`, hook `RegQueryValueExW`,
    redirect query `Shell` → `C:\ex7test\explorer.exe`, fail-safe se manca);
  - `ex7-fake-explorer-path.wh.cpp` (`explorer.exe`, hook
    `GetModuleFileNameW` per `hModule==NULL` → `%SystemRoot%\explorer.exe`);
  - procedura test + reversibilità in `windhawk-POC-README.md`. Obiettivo:
    la **taskbar Windows 7** come shell attiva al login, senza toccare
    `C:\Windows\explorer.exe` né il registry.

## Uso

1. scaricare `ex7selfcontained.exe` e `wrp64.dll` nella stessa cartella
   (es. una vuota `ex7test`);
2. `ex7selfcontained.exe` (doppio click o da prompt);
3. atteso: `cache\explorer-*.pris`, `explorer.exe` patchato **e localizzato
   (en-US + it-IT)**, `wrp64.dll`, `state\install.json`, `log\ex7setup.log`.

Nessun file utente aggiuntivo richiesto. Con OS in lingua diversa da
it/en la shell sarà en-US (fallback voluto).

## Sicurezza

- Solo HTTPS su host Microsoft fissato; identità multilivello documentata in
  `config.h` (due varianti Authenticode note, allow-list);
- nessun binario Microsoft (né `.mui`) negli artifact o nella release;
- catalogo/testi/strutture: 100% authoring del progetto (vedi tabella
  copyright in `installer/ex7selfcontained/README.md`).
