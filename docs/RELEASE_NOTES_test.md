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



## Update 2026-09-28 (v0.0.3-test4)

- **Tutto in uno ZIP**: nuovo asset `ex7-test-bundle.zip` (binari +
  windhawk sorgenti + README); hash incluso in `SHA256SUMS.txt`.
- **Fake-path integrato in `wrp64.dll`** (nessun Windhawk richiesto per il
  runtime): hook MinHook di `GetModuleFileNameW` con **filtro call-site** —
  solo il codice di `explorer.exe` riceve `%SystemRoot%\explorer.exe`;
  wrp64.dll stesso e gli altri moduli vedono il path reale (più preciso del
  mod Windhawk, che spoofava tutto il processo). I sorgenti dei mod
  Windhawk restano in `windhawk/` come alternativa/documentazione.
- **Switcher**: niente più hardcode — path Explorer7 = ① env
  `EX7_EXPLORER_PATH` → ② `explorer.exe` nella STESSA cartella dello
  switcher → ③ fallback `C:\ex7test`; pulsante **Browse…**; opzione login
  **file-based** (link nella cartella Esecuzione automatica UTENTE, nessun
  registry/Winlogon) con checkbox; modalità a riga di comando
  (`--apply-ex7`, `--apply-native`, `--install-login`, `--uninstall-login`).
- **Nota trasparenza upstream**: `world-windows-federation/explorer7` ha
  chiuso il supporto a Windows 11 ("End of support for Windows 11",
  ultimi commit; il nostro base = upstream tip `5885b80`). Su Win11 24H2
  alcuni componenti della shell di Win7 (apertura Start Menu, flyout
  orologio, "Cambia data e ora") dipendono dal wrapper e NON sono coperti
  upstream: sono limitazioni note del base explorer7 su 24H2, non del
  bootstrap. Funzionalità completa prevista su Windows 10 22H2.
- Verificato: nessun file mancante rispetto al repository originale
  (diff completo: solo le nostre aggiunte). Menu/dialog internationalized
  via MUI standard: con sistema en-US i testi restano en-US; it-IT appare
  con Windows in italiano. Stringa 857 ("Almost there") del catalogo: dato
  di struttura a bassa confidenza (v. BOZZE_CONFIDENZA), marcata per
  revisione — non è testo Microsoft.

## Update 2026-09-28 (v0.0.3-test3)

- **Nuovo tool**: `7explorer-shell-switcher.exe` — shell switcher runtime GUI
  nativa Win32 (nessun framework, CRT statico `/MT`). Scambia la shell in
  pochi secondi fra `%SystemRoot%\explorer.exe` e l'Explorer7 privato
  (`C:\ex7test\explorer.exe`, override via variabile `EX7_EXPLORER_PATH`),
  **senza logout/reboot** e senza mai toccare registry/Winlogon/system files.
  - Identifica la shell dall'owner di `GetShellWindow()` e il path con
    `QueryFullProcessImageNameW` → immune allo spoof Windhawk; ferma SOLO
    quel processo (WM_QUIT, poi terminate dopo timeout).
  - Safety: target verificato prima di fermare nulla; se Explorer7 non parte,
    ripristina automaticamente la shell nativa; messaggio critico con recovery
    manuale se fallisce anche quella; warning pre-applicazione.
  - Uso: vedi sezione nel README radice / `switcher/README.md`.
- Bootstrap, pipeline risorse, wrapper e mod Windhawk **immodificati**.

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
