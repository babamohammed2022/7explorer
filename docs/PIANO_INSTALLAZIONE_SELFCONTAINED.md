# Piano: installazione completamente self-contained di explorer7

Data analisi: 2026-09-28. Legenda stato di ogni affermazione:

- ✅ **VERIFICATO NEL SORGENTE** — letto direttamente nel codice di questo
  repo (citazione file:riga).
- 🧪 **VERIFICATO NEL SANDBOX** — provato con test eseguiti dall'agente
  (24/24 verdi al momento della scrittura: `python3 tests/run_tests.py`).
- 🌐 **FONTE ESTERNA NOTA** — non ricontrollabile dal sandbox.
- ⚠️ **DA VERIFICARE SULLA MACCHINA UTENTE** — il sandbox non ha accesso a
  `msdl.microsoft.com` (TLS reset; GitHub sì). Comandi pronti riportati sotto.

---

## 1. Download e verifica di explorer.exe

I valori forniti dall'utente:

| Valore | Costante | Stato |
| --- | --- | --- |
| TimeDateStamp `0x4CE7A144` | `cfg::kTimeDateStamp` | ✅ **CONFERMATO 2026-09-28** dall'utente su Windows 10 21H2 LTSC (19044), download reale + `certutil`; ri-verificato ad ogni run del CI |
| SizeOfImage `0x2C0000` | `cfg::kSizeOfImage` | ✅ **CONFERMATO 2026-09-28** (utente + `--dump-headers` nel CI) |
| SHA-256 `5769…e21b` | `cfg::kExpectedSha256` | ✅ **CONFERMATO 2026-09-28** (utente, `certutil`); nel CI confronto con `Get-FileHash` |
| dimensione 2.872.320 byte | `cfg::kExpectedFileBytes` | ✅ **CONFERMATO 2026-09-28**; controllo esatto in `CheckPeIdentity` |
| URL `…/explorer.exe/4CE7A1442C0000/explorer.exe` | template in `config.h` | ✅ raggiunto e scaricato da utente e CI (l'errore TLS era solo della rete sandbox) |

**Comandi di verifica da eseguire sulla tua macchina (prima del rilascio),**
e output atteso da incollare/verificare:

```bat
curl.exe -L -o %TEMP%\explorer-ref.exe "https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe"
certutil -hashfile %TEMP%\explorer-ref.exe SHA256
python tools\analyze_mui.py %TEMP%\explorer-ref.exe --dump-headers
```

`certutil` deve stampare esattamente l'hash atteso; `--dump-headers` stampa
TimeDateStamp/SizeOfImage/macchina, la presenza della risorsa `MUI` e la
lista degli import — conferma così anche quali di `SHLWAPI.DLL`,
`OLE32.DLL`, `EXPLORERFRAME.DLL` esistono davvero (EXPLORERFRAME è
condizionale, vedi task 2).

Implementato: `installer/ex7selfcontained/downloader.cpp` (WinInet con
timeout per fase, deadline complessiva 120 s, cancellazione immediata a
logoff/shutdown via `SetConsoleCtrlHandler`, 3 tentativi con backoff, cap
16 MB, file temporaneo → hash → `MoveFileEx`), `winhash.cpp` (SHA-256 CNG
sull'handle già aperto con `FILE_SHARE_READ|FILE_SHARE_DELETE`; controllo
identità PE: AMD64, PE32+, TimeDateStamp, SizeOfImage; `WinVerifyTrust`
come controllo secondario NON bloccante sul file *pristine* — la firma è
inevitabilmente invalidata da qualunque patch, ✅ conseguenza logica).
L'hash del file **originale è sempre verificato PRIMA della patch**; l'hash
della **copia patchata è memorizzato in `state\install.json`** a ogni
installazione (rilevamento di copie locali corrotte ai riavvii: la cache
`.pris` è riverificata a ogni run — ✅ logica implementata in
`EnsurePristineExplorer`).
Rifiuto totale di qualunque file non identico: nessun fallback "prova
comunque", log esplicito `HASH MISMATCH` e cancellazione del file.

## 2. Patch degli import (addio CFF Explorer)

✅ **VERIFICATO NEL SORGENTE** (`README.md`, "Step 2 - Patching
explorer.exe"): oggi l'utente deve sostituire a mano gli import
`SHLWAPI.DLL`, `OLE32.DLL` e (se presente) `EXPLORERFRAME.DLL`.

Implementazione: `tools/patch_imports.py` (riferimento multipiattaforma) e
porting 1:1 `installer/ex7selfcontained/importpatch.cpp`.

**È deterministica? Sì 🧪** — e lo dimostro così:

- `tests/test_patch_imports.py` (10 test) costruisce PE32+ sintetici con
  import table e verifica: stessi byte in uscita a ogni esecuzione;
  **idempotenza** (ri-patch di un file già patchato = zero modifiche, log
  azioni vuoto); solo i 3 slot dei nomi DLL bersaglio più il campo
  CheckSum cambiano (test di diff per-offset); i nomi più lunghi
  (`EXPLORERFRAME.DLL` 17+1 B, `SHLWAPI.DLL` 11+1 B) sono paddati a zero
  fino alla lunghezza originale ⇒ **dimensione file invariata**;
  confronto nome **case-insensitive** (come il loader Windows);
  directory dei *bound import* azzerata (i loro nomi vivono in una tabella
  separata: azzerare la directory è la via documentata e sicura);
  campo **CheckSum** ricalcolato con l'algoritmo standard della famiglia
  MapFileAndCheckSum (test che il checksum memorizzato coincide col
  ricalcolo);
  rifiuto di input non-PE/non-AMD64.

Come testarla sulla macchina:

```bat
python tools\patch_imports.py explorer-pristine.exe out1.exe
python tools\patch_imports.py explorer-pristine.exe out2.exe
fc /b out1.exe out2.exe          :: identici = deterministico
python tools\patch_imports.py out1.exe out1b.exe   :: "nothing to do" = idempotente
ex7selfcontained --selftest-importpatch explorer-pristine.exe
   :: (da implementare sul ramo: confronta byte-per-byte col risultato Python)
```

Nota onestà: il porting C++ è **revisionato a vista ma non compilato**
(non c'è toolchain Windows nel sandbox); la logica è identica al riferimento
Python testato 🧪. Da fare una cross-build di prova alla PR successiva.

⚠️ ipotesi da confermare col `--dump-headers` di cui sopra: il `SHLWAPI` del
binario Win7 SP1 ha **anche import per ordinale** (il wrapper infatti
esporta per ordinale, ✅ visibile in `forwards.h` `FORWARDO(SHLWAPI,…)`);
la patch tocca solo il *nome della DLL* nei descrittori, non i thunk: gli
import per ordinale funzionano invariati verso `wrp64.dll` finché il
wrapper esporta gli stessi ordinali — ✅ coerente col progetto esistente.

## 3. Come caricano OGGI le risorse explorer.exe.mui / shell32.dll.mui

✅ **VERIFICATO NEL SORGENTE**:

1. **shell32.dll.mui** — caricato **dal wrapper**, non dal gestore MUI di
   Windows. `StartMenuPin.cpp:14-46` (`Shell32_LoadString`): il wrapper
   aggancia, nella IAT di shell32.dll, l'import `LoadStringW` (via
   `api-ms-win-core-libraryloader-l1-2-0.dll`,
   `StartMenuPin.cpp:241`, `h_shell32`); per i soli ID
   `0x1505, 0x1506, 0x1508, 0x1509` (5381/5382/5384/5385) con
   `hInstance == shell32` carica
   `<dir exe>\<lingua preferita utente>\shell32.dll.mui` con
   `LoadLibraryEx(…, LOAD_LIBRARY_AS_DATAFILE)` e se la stringa non si
   trova fa fallback a `LoadStringW(g_hInstance,…)` = **risorse di
   wrp64.dll stesso**. `wrapper.rc` contiene già le 4 stringhe inglesi.
   Contesto d'uso: testi "pin/unpin" del menu Start (serve perché su Win ≥ 8
   gli ID cambiarono; cf. anche README, nota Windows 8.1 "Customize Start
   Menu"). Quindi: **tipologie davvero usate da explorer7 solo queste 4
   stringhe** — niente menu/dialog di shell32.
2. **explorer.exe.mui** — caricato dal **gestore MUI del kernel**: il
   Win7 `explorer.exe` è un PE language-neutral con risorsa `RCDATA "MUI"`;
   i suoi `LoadString/LoadMenu/LoadDialog/LoadAccelerators` passano dal
   resource loader che cerca `<dir exe>\<lingua>\explorer.exe.mui`. ✅ coerente
   col layout del README (cartella `en-US` accanto a `explorer.exe`) e con
   l'uso di `GetUserPreferredUILanguages` nel wrapper per costruire i path.
   Il parsing MUI con validazione incrociata dei checksum (LN↔mui) è il
   motivo per cui l'opzione (a) costerebbe una reimplementazione alla
   muirct: scartata come strada principale (vedi sotto). — Dichiarato come
   conoscenza di dominio, 🌐 non ri-testata qui.

### Scelta: opzione (b) ibrida ✅ motivata

- **explorer.exe (copia privata)**: neutralizzazione della risorsa
  `MUI` → `CUI` (stesso trucco della tua mod B) e **iniezione** nel binario
  delle STRINGTABLE per lingue del catalogo (`localizer.cpp`,
  `Begin/Update/EndUpdateResource`). **Cancelletto di sicurezza**: la
  neutralizzazione è rifiutata finché non esiste
  `localization/constraints/explorer.exe.constraints.json` (altrimenti la
  shell resterebbe mezza localizzata) — `--allow-partial-localization`
  per test espliciti.
- **shell32**: **zero file generati**: `tools/embed_catalog.py` produce
  `explorerwrapper/ex7_languages.rc` (aggiunto al vcxproj) con le
  STRINGTABLE in tutte le lingue del catalogo; il fallback già esistente in
  `StartMenuPin.cpp` le serve automaticamente nella lingua UI.
  `ex7selfcontained` può anche generare un `shell32.dll.mui` ridotto
  (PE solo risorse) — il wrapper lo caricherebbe come datafile senza
  validazione incrociata (🌐: nessun checksum MUI coinvolto su quel path) —
  lasciato come miglioria futura non necessaria.
- Nessun hook `LoadString` aggiuntivo per explorer: a MUI neutralizzata e
  stringhe iniettate, i `LoadString` leggono direttamente dal binario.

### ID davvero necessari

- ✅ shell32.dll.mui: **5381 5382 5384 5385** (verificati in sorgente).
- ⚠️ explorer.exe.mui: insieme completo NON inventabile — va estratto dal
  file di riferimento con `tools/analyze_mui.py`. Comandi:

```bat
python tools\analyze_mui.py explorer.exe.mui --constraints localization\constraints\explorer.exe.constraints.json
python tools\analyze_mui.py explorer.exe.mui --with-strings > %TEMP%\ref-strings.json   :: NON committare
```

  Il primo (solo struttura: ID, lunghezze, acceleratori, segnaposto,
  geometria menu/dialog) si committa; il secondo (testo di riferimento) no.

## 4. Contenuto linguistico & copyright

Regole implementate e 🧪 testate:

- i testi nel repo sono **riformulazioni originali** in 10 lingue
  (`localization/catalog/*.json`), nessun testo Microsoft riportato; la
  struttura (ID, tipi, segnaposto, lettere acceleratore, lunghezze di
  riferimento) è esterna al copyright e proviene dall'analizzatore.
- `tools/verify_catalog.py` (CI-ready, esce 1 al primo errore): segnaposto
  per numero/ordine/tipo (`%s`, `%d`, `%1!s!`, `%%`, `%I64u`, `%ls`, `%1`…);
  **un solo `&` per stringa**; **unicità acceleratori** per contesto
  coesistente (menu/dialog); budget di lunghezza (max 2× o +8 caratteri
  rispetto al riferimento, con override solo commentato);
  **inglese obbligatorio e completo come fallback**.
- Catalogo attuale: le 4 stringhe shell32 × 10 lingue (en, it, de, fr, es,
  pt-BR, pl, ru, ja, zh-CN) — verifiche strutturali 0 errori.
- **Nessun binario Microsoft nel repo**; il file mediaexplorer.exe.mui di
  riferimento (mediafire) **non viene scaricato/committato**; solo la sua
  *struttura* confluisce nei constraints JSON.

**Lingue da far revisionare prima a un madrelingua (priorità):**
1. **ja** e **zh-CN** (registro e scelta degli acceleratori `(&X)` nei menu);
2. **ru** (uso delle «» e registro);
3. **de / fr / es** (coerenza tono-imperativo);
4. pt-BR, pl secondarie; en/it già riviste in questa sede.

Sul punto "se una scelta comporta incorporare testi Microsoft": con questa
architettura **non serve**. L'unico testo Microsoft presente nel repo è
quello già ereditato dall'upstream in `wrapper.rc` (le 4 stringhe inglesi):
sterilizzarlo non è nella mia scope senza una tua decisione — se vuoi,
sostituisco anche quelle con le riformulazioni del nostro catalogo (2 righe
di RC) — dimmelo tu.

## 5. Robustezza

| Requisito | Dove | Stato |
| --- | --- | --- |
| Mai bloccare logon/shell | timeout WinInet per fase + deadline 120 s + cancellazione su CTRL_LOGOFF/SHUTDOWN; nessuna attesa utente | ✅ implementato (non compilabile qui) |
| Rete assente / riuso offline | cache `.pris` riverificata con hash a ogni run; `--offline` forza | ✅ implementato |
| Niente MAX_PATH | prefisso `\\?\` su path lunghi (`OpenForReadShared`) | ✅ implementato |
| Log leggibili | `log\ex7setup.log` UTF-16 BOM + stdout | ✅ implementato |
| Non toccare file di sistema | tutto sotto `--app-dir`; nessun accesso a `%SystemRoot%` | ✅ per costruzione |
| Nessun binario MS nel repo | solo struttura JSON + testo originale | ✅ |
| Test target | Windows 10/11: da eseguire (⚠️ checklist sotto) | 🧪 suite logica OK qui |

### Checklist di test su macchina reale (Win10 e Win11)

1. `certutil`/`--dump-headers` confermano le 3 costanti → se no, stop.
2. Run a rete: scarica → hash OK → patch → `install.json` con entrambi gli
   hash → log senza errori.
3. Run `--offline` a rete spenta: riusa cache in <1 s.
4. Cancellazione durante download → abort immediato, nessun file cache.
5. `fc /b` tra due run → byte-identici (fino a install.json, che contiene
   hash stabili).
6. Shell: pin/unpin nel menu Start nella lingua UI; fallback en per lingue
   assenti; verifica UI dei testi troppo lunghi (warning del verifier).

## 6. Stato dell'arte delle verifiche (riepilogo onesto)

- 🧪 Fatto qui: patch deterministica+idempotente; analizzatore risorse su
  fixture; verificatore catalogo (inclusi casi negativi); generazione
  header/RC deterministica (due run consecutivi byte-identici);
  non raggiungibili dal sandbox: msdl.microsoft.com, mediafire.
- ✅ Confermato dall'utente (2026-09-28, Win10 21H2 LTSC 19044): URL,
  dimensione 2.872.320 byte, SHA-256, TimeDateStamp, SizeOfImage.
- 🔄 Nel CI (`selfcontained-ci.yml`): ricontrollo identity sul file reale,
  patch Python↔C++ byte-per-byte, import risultanti verso wrp64.dll; build
  MSVC di wrp64.dll e ex7selfcontained.exe; test Python.
- ⚠️ Ancora aperti: elenco ID `explorer.exe.mui` (stringhe/menu/dialog/
  acceleratori) — in arrivo dall'utente; test comportamentali Win10/11 su
  macchina reale; integrazione shell (fuori scope per questa tappa).
