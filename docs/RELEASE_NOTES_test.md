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




## Update 2026-09-28 (v0.0.3-test6)

- **Switcher bilingue**: UI in inglese di default, italiano se il sistema è
  italiano (override env `EX7_LANG=it|en` o `--lang it|en`).

## Update 2026-09-28 (v0.0.3-test8)

- **REGRESSIONE RIMOSSA — tema**: il fallback automatico di test7 verso
  l'`aero.msstyles` di Windows 10/11 è stato **completamente eliminato**
  (mescolava classi di tema moderne sulla shell Win7: ilegibile). Il wrapper
  torna al comportamento upstream: cerca SOLO `<cartella>\theme\<nome>.msstyles`
  (default `aero`) — se non c'è, tema classico leggibile.
- **Pulsante "Tema…" / "Theme…" nello switcher** (sostituisce il defunto
  "Menu dump"): selezioni **il TUO** `aero.msstyles` di Windows 7 (es.
  estratto dal tuo `aero.rar` o da un tuo disco di Windows 7) e lo switcher
  lo copia in `<cartella-explorer7>\theme\aero.msstyles`; se accanto al file
  c'è anche `<lingua>\aero.msstyles.mui` (en-US e/o it-IT) copia pure quella.
  **Copia 100% locale del TUO file**: il progetto non scarica, non include e
  non ridistribuisce asset di terzi; il copyright del tema resta tuo sul tuo
  PC (è lo stesso meccanismo documentato upstream). Dopo la copia: Cambia →
  Explorer7 (o passa a nativa e torna a Explorer7) per vedere il tema Win7.
- **Menu contestuale taskbar RISCRITTO (menu 12000) con struttura Windows 7
  reale e command ID documentati pubblicamente** (Code Project / AutoHotkey /
  documentazione Shell.Application: 403 Cascade, 404 stacked, 405 side by
  side, 407 desktop, 420 Task Manager, 424 Lock, 413 Properties):
  &Toolbars ▸ / Ca&scade windows / Show windows stac&ked / Show windows
  s&ide by side / Show the &desktop / Start Task &Manager /
  &Lock the taskbar / P&roperties — testi EN+IT (nostra composizione).
  La struttura precedente era sbagliata (voci annidate dentro "Toolbars" e
  ID comando errati) — causa del menu "solo Toolbars".
- **"Help and Support"**: la stringa 7021 ("Guida e supporto" IT) è presente
  e verificata nel payload IT; ritestare con questa build.
- **Voce "Personalizza…" del flyout area di notifica**: NON è controllabile
  da noi — quel flyout lo disegna la shell32 **di Windows 10** in esecuzione
  (testi dell'OS), non il binario Win7. Cambiarlo richiederebbe patchare
  file di sistema: fuori scope per scelta del progetto.
- Rimosso il probe "Menu dump" (non funzionante; non più necessario ora che
  il menu è hardcoded nella forma corretta).

## Update 2026-09-28 (v0.0.3-test7)

- **Fix tema "classico"**: trovata la causa — il ThemeManager del wrapper
  cercava SOLO `<cartella-exe>\theme\aero.msstyles`, assente nel bundle, e in
  fallback restava il tema classico (menu contestuali, tray, start in stile
  Windows 95). Ora, se il file `theme\aero.msstyles` manca, il wrapper tenta
  `%WinDir%\Resources\Themes\aero\aero.msstyles` del sistema in esecuzione
  (file locale dell'utente: niente asset Microsoft ridistribuiti).
  Risultato atteso su Win10/11: taskbar/start/menu con resa **aero moderna**
  invece del classico. Nota: gli `.msstyles` di 8.1+ non contengono le classi
  start-menu di Win7, quindi lo start menu resterà stilato "moderno".
  **Per l'aspetto Win7 completo**: copiare da un proprio Windows 7 il file
  `aero.msstyles` e la sua cartella `en-US` in `theme\` accanto a
  `explorer.exe` (layout identico a upstream explorer7) — non possiamo
  includerlo noi (asset Microsoft non ridistribuibile).
- **Switcher: nuova diagnostica "Menu dump"** (bottone in basso, o
  `7explorer-shell-switcher.exe --dump-menus`): apre per un istante il menu
  contestuale **reale** della taskbar della shell attiva, ne legge voci/ID/
  sottomenu e li scrive in `7explorer-menudump.txt` (accanto allo switcher),
  poi lo richiude. Non tocca registry né file di sistema.
  **Richiesta di test**: con shell **Explorer7** attiva fare "Menu dump" e
  mandarci il file `7explorer-menudump.txt`; ripetere opzionale anche con la
  shell **nativa** (per confronto). Ci serve la struttura vera (di sola
  lettura, dal vivo) per completare le voci del menu taskbar mancanti
  ("Cascade windows", "Show the desktop", "Task Manager", ...).
- CI: sonda strutturale del binario di riferimento (solo forme, nessun
  testo estratto) — ha confermato che `explorer.exe` di Win7 non contiene
  menu/stringhe nel binario principale: tutto vive nella MUI-chain.

## Update 2026-09-28 (v0.0.3-test5)

- **Catalog stringhe rifatto su fonte autorevole** (estratto MUI reale
  fornito dall'utente con ID Win32 veri + len + acceleratori + placeholder):
  i vecchi ID erano shiftati in molt*. tabelle → testi sbagliati in UI
  ("Almost there" nell'overflow, "Riavvia" al posto di Control Panel,
  tooltip errati). Ora: 857 = "Show desktop", 852 = "Clock", 8234 =
  "Control Panel", 205/211/212/12000 menu corretti, tooltip Aero Peek e
  Jump List corretti con placeholder `%s`/`%1` verificati in CI.
- **Italiano riformulato** (nostro, non testo MS) per tutte le 201 stringhe.
- verify_catalog: 0 errori; build_resources: 90 payload (2 lingue);
  embed deterministico; suite 43/43.

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

## Update 2026-09-28 (v0.0.3-test9)

- **Tema Win7-like 100% AUTOSUFFICIENTE, DISEGNATO DA ZERO**: nessun file
  richiesto all'utente, nessun asset Microsoft, nessun download.
  - `tools/theme/` genera un `.msstyles` originale del progetto (formato
    ricostruito via reverse-engineering strutturale del formato: CMAP a
    separatore singolo + regione riservata `documentation/sizevariant.*/
    colorvariant.*/globals/sysmetrics`, VMAP/RMAP/BCMAP esatti, record
    proprietà verificati contro il parser uxtheme reale — accettato:
    `GetThemeDefaults=S_OK` in CI, unsigned).
  - Lo stile è volutamente **molto simile a Win7** (gradiente vetro scuro
    sulla taskbar 40px, menu chiari con selezione blu, StartPanel
    bianco/azzurrino, Segoe UI 9pt nei record FONT reali — palette e
    metriche sono authoring originale, non valori Microsoft).
  - Il blob è **embedded nella `wrp64.dll`** (risorsa RCDATA, 12,8 KB):
    al primo avvio viene auto-estratto in
    `%LocalAppData%\7explorer\theme\aero.msstyles` (se non presente) e
    caricato dal percorso upstream esistente. Se il sistema non lo
    accetta, **fallback silenzioso al look precedente** — nessuna
    regressione, nessun prompt. Diagnostica (senza interazione utente) su
    `%LocalAppData%\7explorer\theme.log`, alfine per segnalazioni GitHub.
  - Gate CI extra: blob rigenerato byte-per-byte da sorgente ad ogni run
    (riproducibilità garantita).
- Sonda CI `tools/theme/probe/ThemeProbe.exe` (solo dev, non rilasciata):
  matrice COMPLETA di decodifica (carve/swap/analyze) documentata in
  `tools/theme/`.
- Menu taskbar, "Help and Support" e resto del bootstrap **immodificati**
  rispetto a test8.

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
