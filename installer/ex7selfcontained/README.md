# ex7selfcontained

Bootstrap self-contained per explorer7: scarica **una sola volta** il binario
Microsoft (`explorer.exe` Win7 SP1 x64) dal symbol server, lo verifica con
SHA-256 fissato nel codice, patcha gli import verso `wrp64.dll` e **inietta
risorse UI generate dal progetto** (stringhe, menu, dialoghi, acceleratori).
Offline dal secondo avvio.

> ## Localizzazione v0.0.3: nessun `.mui` richiesto, MAI
>
> L'utente **non deve procurarsi nessun `explorer.exe.mui`**: non serve
> copiarlo accanto all'installer, non esiste più la variabile
> `EX7_REFERENCE_MUI`, non c'è più nessun controllo di identità sul `.mui` e
> non si interroga alcun symbol server per i `.mui`. Il vecchio meccanismo di
> "trapianto" (v0.0.2) è stato rimosso dopo i test reali (root cause in
> `localizer.h` e nei log CI: `UpdateResource` rifiuta le scritture nel
> binario LN marcato MU con `ERROR_NOT_SUPPORTED` 50; la rimozione del solo
> marcatore con `ERROR_INVALID_PARAMETER` 87; l'unica via accettata è la
> riscrittura completa della tabella risorse).
>
> La nuova pipeline è interamente del progetto:
>
> ```
> localization/catalog/en.json, it.json        <- testi NOSTRI (GPL, del progetto)
> localization/templates/*.templates.json      <- struttura (ID, stili, rect,
>                                                 classi, cmd): nessun testo MS
>                 |
>        tools/build_resources.py              <- genera i PAYLOAD PE reali
>                 |                             (STRINGTABLE/MENU/DIALOGEX/ACCEL)
>                 |                             e li RIVALIDA parsandoli indietro
>        tools/embed_catalog.py                <- embed in lang_catalog.h (blob)
>                 |
>        ex7selfcontained.exe                  <- a install time: riscrittura
>                                                 atomica della tabella risorse
>                                                 della copia privata (tutte le
>                                                 risorse esistenti copiate,
>                                                 "MUI" -> "CUI", blob iniettati)
> ```
>
> - **Lingue**: `en-US` (sempre presente, è il fallback) e `it-IT`. Un OS con
>   un'altra lingua ottiene en-US. Aggiungere una lingua = nuovo
>   `catalog/<id>.json`; la struttura è condivisa.
> - **Validazione pre-flight** (fallimento = stop netto, mai shell a metà):
>   `tools/verify_catalog.py` (ID, tipi, conteggi, placeholder, acceleratori
>   doppi/mancanti, coerenza en↔it, fallback coverage) +
>   round-trip in `build_resources` (ogni payload generato viene riparsato e
>   confrontato su struttura e testo) + `tools/check_pe_resources.py` (prova
>   sul file prodotto: presenza byte-per-byte, parsing, MUI neutralizzato).
> - **Fallback di errore**: se la generazione o l'iniezione fallisce,
>   l'installazione si ferma con errore esplicito (exit 1) e la transazione
>   risorse viene SCARTATA. Non esiste più `--allow-partial-localization`.
>
> ### Copyright / cosa c'è nel repo
>
> | Contenuto | Origine | Distribuito? |
> |---|---|---|
> | codice installer + tools Python | progetto | sì (GPLv3) |
> | testi in `localization/catalog/*.json` | progetto | sì |
> | `templates/*.templates.json` (solo struttura: ID, numeri, layout) | derivato da analisi strutturale, zero testo Microsoft | sì |
> | `explorer.exe` Microsoft | symbol server Microsoft, scaricato a installazione | MAI (né in repo, né in CI artifact, né in release) |
> | qualsiasi `.mui` Microsoft | — | MAI (e non serve) |
>
> Ricostruzione strutturale non garantita? Il template descriptor è marcat
> `BEST-EFFORT` per i bit di stile finestre/controlli (derivati dai vincoli +
> default Win32); la riconciliazione contro il file di riferimento è fatta
> con `analyze_mui.py --templates` + `build_resources.py --compare`
> (solo struttura, mai testo). Vedi `localization/templates/…._notice`.

Vedi `docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md` per l'analisi completa e lo
stato di verifica di ogni costante.

## Build (Windows, con Visual Studio 2022 o il toolset v143)

```bat
python tools\embed_catalog.py
msbuild installer\ex7selfcontained\ex7selfcontained.vcxproj /p:Configuration=Release /p:Platform=x64
```

oppure da "x64 Native Tools Command Prompt":

```bat
cd installer\ex7selfcontained
cl /std:c++17 /utf-8 /O2 /W4 /EHsc main.cpp downloader.cpp winhash.cpp importpatch.cpp localizer.cpp /link bcrypt.lib wintrust.lib crypt32.lib wininet.lib
```

`lang_catalog.h` è GENERATO: non modificarlo a mano.

## Verifica delle costanti Microsoft — FATTA (2026-09-28)

Valori confermati dall'utente su Windows 10 21H2 LTSC (19044) e riverificati
ad ogni run del CI (`selfcontained-ci`, step "Download reference explorer.exe
and re-verify pinned identity"). Per riverificarli a mano:

```bat
curl.exe -L -o %TEMP%\\explorer-ref.exe "https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe"
certutil -hashfile %TEMP%\\explorer-ref.exe SHA256
python tools\\analyze_mui.py %TEMP%\\explorer-ref.exe --dump-headers
```

`certutil` deve stampare uno dei due hash allow-listati (varianti documentate
in `config.h`), e `--dump-headers` deve mostrare `TimeDateStamp 0x4CE7A144`,
`SizeOfImage 0x2C0000`, machine `0x8664` e gli import
`SHLWAPI.DLL`, `OLE32.DLL`, `EXPLORERFRAME.DLL`.
Se qualunque valore differisce, NON eseguire l'installer e segnalalo.

## Esecuzione

```bat
ex7selfcontained.exe [--app-dir "X:\Program Files\explorer7"] [--offline] [--skip-signature]
```

- primo avvio: scarica + verifica + patch + riscrittura risorse localizzate;
- avvii successivi (anche con `--offline`): riusa `cache\explorer-*.pris`
  riverificandone l'hash; niente rete se l'hash corrisponde.

Mai eseguire a logon in modo bloccante: deadline complessive, cancellazione
immediata a logoff/shutdown e nessuna scrittura fuori dalla cartella app.

## Test

| Livello | Dove | Cosa prova |
|---|---|---|
| `python tests/run_tests.py` | qualunque OS (43 test) | patcher byte-level, analyzer, verifier, **build_resources round-trip** (menu/dialog/string/accel), compare template |
| `python tools/verify_catalog.py --catalog-dir localization/catalog --constraints …` | qualunque OS | coerenza cataloghi ↔ vincoli, copertura fallback |
| `python tools/build_resources.py` | qualunque OS | generazione 88 payload (44/lingua) + riparsa |
| `selfcontained-ci` (GitHub Actions, windows-latest) | **Windows reale** | build MSVC; download file reale; hash; patch Py vs C++ byte-a-byte; **run end-to-end dell'installer + `check_pe_resources.py` sul PE prodotto + rerun idempotente** |
| richiede Windows reale, non in CI | manuale | avvio effettivo della shell patchata (taskbar/start menu resi, dialoghi non tronchi a video, layout reali): l'exe modificato non può essere *eseguito* sui runner (solo analizzato) |
