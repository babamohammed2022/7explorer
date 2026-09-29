# Installazione

Guida completa all'installazione, dall'archivio al primo avvio (e ai
successivi, offline). Per i problemi noti: [troubleshooting.md](troubleshooting.md).

Indice: [requisiti](#requisiti) · [procedura](#procedura) · [cosa viene scaricato-e-verificato](#cosa-viene-scaricato-e-verificato)
· [dove finiscono i file](#dove-finiscono-i-file) · [avvio-offline](#avvio-offline)
· [disinstallazione](#disinstallazione) · [hidpi-e-limiti](#hidpi-e-limiti)

---

## Requisiti

- **Windows 11 24H2 / 25H2, x64** (target attivo). Windows 8.1/10 x64
  funzionano in larga parte ma non sono il focus.
- Connessione Internet **solo al primo avvio** (vedi
  [tabella](#cosa-viene-scaricato-e-verificato)); dal secondo avvio tutto
  è offline.
- **Nessuna elevazione**: l'installazione è per-utente, non tocca file di
  sistema (mai `C:\Windows\explorer.exe`, mai `HKLM`).

## Procedura

1. Scarica **`ex7-test-bundle.zip`** dalla [release di riferimento]
   (https://github.com/babamohammed2022/7explorer/releases) — contiene
   `wrp64.dll`, `ex7selfcontained.exe`, `7explorer-shell-switcher.exe` e i
   README (inclusi i sorgenti dei mod Windhawk opzionali).
2. Decomprimi tutto in **una singola cartella**, ad es. `C:\ex7test`.
3. Avvia **`ex7selfcontained.exe`**:
   - scarica `explorer.exe` (Windows 7 SP1 x64) dal symbol server
     Microsoft e ne verifica **SHA-256, dimensione, TimeDateStamp e firma
     Authenticode** prima di usarlo;
   - copia in `explorer.exe` privato e patcha gli import
     (`SHLWAPI.DLL`, `OLE32.DLL`, `EXPLORERFRAME.DLL` → `wrp64.dll`);
   - genera e inietta le risorse UI (en-US + it-IT) prodotte dal progetto:
     **nessun file `.mui`** viene richiesto, scaricato o creato.
   L'operazione è deterministica e ripetibile: rieseguirla riscrive lo
   stesso file (verificato dalla CI a ogni build).
4. Avvia **`7explorer-shell-switcher.exe`** → **Windows 7 Explorer** →
   **Cambia**. La shell Win7 sostituisce quella di Windows 11 al volo.
5. Per tornare: `Ctrl+Alt+Shift+S` → **Esplora risorse Windows nativo** →
   **Cambia**.
6. (Opzionale) **"Avvia Explorer7 automaticamente al logon"** nel omonimo
   switcher: [avvio-al-login.md](avvio-al-login.md).

## Cosa viene scaricato e verificato

Trasparenza totale: **ogni file scaricato ha URL e SHA-256 fissati nel
codice** (non negoziabili a runtime); un file con hash imprevisto viene
scartato. Nessun binario Microsoft viene mai ridistribuito dal progetto:
il download avviene sempre dai server originali (symbol server Microsoft o
repository ExplorerPatcher per i soli file `.mui` di pnidui).

### All'installazione (`ex7selfcontained.exe`, primo avvio)

| file | URL | SHA-256 (allow-list) |
|---|---|---|
| `explorer.exe` Win7 SP1 x64 (6.1.7601.17514, 2.872.320 byte, TimeDateStamp `0x4CE7A144`, SizeOfImage `0x2C0000`) | `https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe` | `5769e5b25c7bfbc20dbfdca2f17b751f6d968e03412705de4a16c99b2626e21b` oppure `6a671b92a69755de6fd063fcbe4ba926d83b49f78c42dbaeed8cdb6bbc57576a` (due copie legittime re-firmate osservate: utente Win10 21H2 LTSC e runner CI; struttura identica, vedi `installer/ex7selfcontained/config.h`) |

L'identità è verificata su **quattro livelli**: hash allow-list,
dimensione in byte, `TimeDateStamp`/`SizeOfImage` e firma Authenticode.
La CI scarica e riverifica lo stesso file a ogni build.

### Al primo avvio della shell (`wrp64.dll`, in background)

| file | URL | SHA-256 |
|---|---|---|
| `pnidui.dll` 10.0.22621.3810 x64 (icona di rete) | `https://msdl.microsoft.com/download/symbols/pnidui.dll/F717CABC20B000/pnidui.dll` | `52e9c88cb50ef98e683839ab62880180f0dd3db3e1c445ea461ae63d97a349d3` |
| `pnidui.dll.mui` **en-US** | `https://raw.githubusercontent.com/valinet/ExplorerPatcher/0a88a6e0ef6b1752fea36e581cffff1097e862b0/ep_setup/resources/files/pnidui/en-US/pnidui.dll.mui` | `ae0d2655cb9806b5c16b92f82d698f5c98feaef1f78ebdd6014dccc8304184ed` |
| `pnidui.dll.mui` **it-IT** | `https://raw.githubusercontent.com/valinet/ExplorerPatcher/0a88a6e0ef6b1752fea36e581cffff1097e862b0/ep_setup/resources/files/pnidui/it-IT/pnidui.dll.mui` | `e7ce6e6e43483815b79946b05e6f744e9277a123ef387485826d558533609f20` |
| `batmeter.dll` 6.3.9600.17415 x64 (flyout batteria) | `https://msdl.microsoft.com/download/symbols/batmeter.dll/545054931f3000/batmeter.dll` | `f32f18d44f9a6511c73ca1a9a4a6edad38aff23a15fd4c75d9aaaaf31526a506` |
| `stobject.dll` 6.3.9600.17415 x64 (flyout batteria) | `https://msdl.microsoft.com/download/symbols/stobject.dll/54503A4356000/stobject.dll` | `30737741f7131ff80706c7d12b2fe8ab8a6203aeaf9984d9a7c908c0f2565149` |

Note:

- i `.mui` di pnidui esistono per ~30 lingue nel codice
  (`explorerwrapper/NetworkIcon.cpp`, tabella `kMui`); qui sono riportati
  en-US e it-IT, le lingue localizzate dal progetto;
- i file del flyout batteria 8.1 vengono scaricati **solo se** il sistema
  ha una batteria e il build è nella tabella dei supportati; dopo un
  fallimento si riprova al massimo una volta ogni 24 h;
- il download avviene in un thread in background: al prim'avvio
  l'**icona di rete non c'è ancora**; compare dal secondo avvio della
  shell (comportamento noto, vedi [troubleshooting](troubleshooting.md)).

## Dove finiscono i file

| percorso | contenuto |
|---|---|
| `<cartella del bundle>\explorer.exe` | la copia privata, patchata e localizzata (il "working copy") |
| `<cartella del bundle>\cache\explorer-<ts>-<soi>.pris` | copia **pristina** verificata dell'explorer Win7 (riusata offline) |
| `<cartella del bundle>\state\install.json` | record di installazione (hash, orari) |
| `<cartella del bundle>\log\ex7setup.log` | log leggibile dell'installer |
| `%LocalAppData%\7explorer\pnidui-F717CABC20B000\` | pnidui + .mui per l'icona di rete |
| `%LocalAppData%\7explorer\w81flyout\` | batmeter/stobject 8.1 per il flyout batteria |
| `%LocalAppData%\7explorer\theme\aero.msstyles` | tema embedded auto-estratto (fallback) |
| `%TEMP%\7explorer-shellfix.log` | log della shell (wrp64.dll) |
| `%TEMP%\7explorer-switcher.log` | log dello switcher (switch, logon, recovery) |
| `%LocalAppData%\7explorer\theme.log` | log del tema |

## Avvio offline

Dal **secondo** avvio non serve alcuna rete:

- `ex7selfcontained.exe` riusa la copia pristina in `cache\` (già
  verificata all'origine) — oppure accetta il flag `--offline` per non
  tentare alcun contatto;
- la shell riusa i componenti in `%LocalAppData%\7explorer\`; se un
  download precedente è fallito, la funzione relativa resta semplicemente
  disattivata (icona di rete assente, flyout batteria di sistema) finché
  non va a buon fine.

## Disinstallazione

1. Deseleziona l'avvio automatico al logon (o
   `7explorer-shell-switcher.exe --uninstall-login`) — ripristina il
   valore `Shell` precedente, elimina link e task di recovery
   ([dettagli](avvio-al-login.md)).
2. Torna alla shell nativa (switcher o `Ctrl+Alt+Shift+S`).
3. Elimina la cartella del bundle e, se vuoi, `%LocalAppData%\7explorer\`.
   Non c'è altro: nessun file di sistema né valore HKLM è mai stato
   toccato.

## HiDPI e limiti

- **HiDPI**: l'explorer Win7 non è DPI-aware come quello di Win11: a
  125%/150% la taskbar e il menu Start usano il raddoppio classico. Il
  sistema di orb supporta immagini dedicate per 125%/150% (vedi README
  upstream nella sezione "Custom orbs").
- La taskbar esiste solo sul **monitor primario** (limite dell'explorer
  Win7).
- Le dimensioni/posizione della taskbar modificate vengono salvate nel
  registro Win7 dopo qualche minuto: riavviare explorer subito dopo può
  annullarle (limite Win7 noto).
