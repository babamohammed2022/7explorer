<p align=center>
  <img src="https://github.com/user-attachments/assets/77a7d7b1-3022-43ab-9c2a-9a09a923be39">
</p>

# 7explorer

**7explorer** è il shell-explorer di **Windows 7** che gira **come shell di
Windows 11** (target: build **24H2 / 25H2**, x64): taskbar, menu Start e
Esplora risorse dell'epoca, nel desktop di oggi.

È un **fork di [explorer7](https://github.com/world-windows-federation/explorer7)**
(World Windows Federation), licenza **GPLv3**. Il cuore è una *wrapper
library* (`wrp64.dll`) caricata da una copia **privata** di `explorer.exe`
di Windows 7 SP1: nessun file di `C:\Windows` viene mai modificato.

> [!IMPORTANT]
> ## ⚡ COME TORNARE SUBITO A WINDOWS 11
> **Premi `Ctrl` + `Alt` + `Shift` + `S`** — apre lo *Shell Switcher* anche
> se la shell Win7 è bloccata o è crashata. Da lì seleziona
> **"Esplora risorse Windows nativo" → Cambia**: il passaggio Win7 ⇄ Win11 è
> **istantaneo, senza logout e senza riavvio**.
>
> È il **pulsante di sicurezza** di tutto il progetto: qualunque cosa vada
> storto con la shell Win7, una pressione ti riporta alla shell di Windows 11.
> Dettagli su quando è attiva: [la scorciatoia di emergenza](#-la-scorciatoia-di-emergenza-ctrlaltshifts).

> [!WARNING]
> **Stato: progetto in fase di test (commit `testNN`, ultimo `test37`).**
> Le funzioni sono reali ma non tutto è stabile: leggi
> [Stato onesto del progetto](#stato-onesto-del-progetto) prima di
> installare, e tieni sempre a portata di mano `Ctrl+Alt+Shift+S`.

---

## Indice

- [⚡ La scorciatoia di emergenza (Ctrl+Alt+Shift+S)](#-la-scorciatoia-di-emergenza-ctrlaltshifts)
- [Stato onesto del progetto](#stato-onesto-del-progetto)
- [Quick start](#quick-start)
- [Requisiti](#requisiti)
- [Avvio automatico al logon](#avvio-automatico-al-logon)
- [Documentazione](#documentazione)
- [Componenti del repository](#componenti-del-repository)
- [Sviluppo e build](#sviluppo-e-build)
- [Convenzione linguistica](#convenzione-linguistica)
- [Licenza e crediti](#licenza-e-crediti)

## ⚡ La scorciatoia di emergenza: Ctrl+Alt+Shift+S

| | |
|---|---|
| **Cosa fa** | Apre (o porta in primo piano) il *7explorer Shell Switcher*, da cui si cambia shell **senza logout e senza riavvio**. Se la shell Win7 è appesa, crashata o assente, la scorciatoia funziona comunque: l'istanza che la possiede è un processo indipendente da explorer. |
| **Quando è attiva** | Quando è in esecuzione l'istanza resident `--hotkey` del switcher (una sola per sessione, protetta da mutex `Local\7explorer.ShellSwitcher.Hotkey`). Viene avviata automaticamente: ① dalla **GUI** dello switcher, ogni volta che la apri; ② da **`wrp64.dll`** al primo avvio della shell Win7 (quindi è attiva **dopo ogni logon** con l'avvio automatico abilitato, senza aprire nulla — verificato nel codice: `StartSwitcherHotkey` in `explorerwrapper/ShellFixes.cpp`); ③ dal task di recovery e dai comandi `--apply-*` dopo uno switch riuscito. |
| **Quando NON è attiva** | Se la shell Win7 non è in esecuzione **e** lo switcher non è mai stato aperto in quella sessione (es. shell nativa dopo un logon senza avvio automatico): nessun processo possiede la scorciatoia e la pressione non fa nulla. Soluzione: apri `7explorer-shell-switcher.exe` una volta. |
| **Disattivabile** | Sì, con l'opzione `SwitcherHotkey=0` (vedi [docs/opzioni.md](docs/opzioni.md)). |

Recovery manuale estrema (nessuna shell sullo schermo): `Ctrl+Shift+Esc` →
Gestione attività → *Esegui nuova attività* → `explorer.exe`.

## Stato onesto del progetto

Il progetto è in **fase di test attiva**: la numerazione dei commit
(`testNN`, ultimo `test37`) è la cronologia delle iterazioni. Cosa funziona
e cosa no, oggi:

**Funziona (usato regolarmente in sviluppo)**

- La shell Win7 parte e resta come shell su Windows 11 24H2/25H2:
  taskbar, menu Start, tray, Esplora risorse, tema Aero.
- **Cambio shell a runtime** Win7 ⇄ Win11 senza logout (switcher +
  scorciatoia Ctrl+Alt+Shift+S).
- **Avvio automatico al logon** (test37): valore `Shell` per-utente,
  reversibile, con fallback e task di recovery automatico —
  [docs/avvio-al-login.md](docs/avvio-al-login.md). *Nota: la matrice di
  test completa logoff/riavvio è in verifica su hardware reale, vedi il
  PR del test37.*
- Installer **self-contained**: scarica l'`explorer.exe` Win7 SP1 dal
  symbol server Microsoft (SHA-256 fissato nel codice), lo patcha e lo
  localizza; **offline dal secondo avvio**.
- Risorse UI (stringhe/menu/dialoghi, en-US + it-IT) **generate dal
  progetto**: nessun file `.mui` Microsoft.
- Menu tray classici (volume, rete), flyout battery in stile 8.1,
  flyout con bordi Aero, Win+I che apre Impostazioni, jump list delle app
  Win32.

**Parziale / noto-rotto (onestamente)**

- **Tema embedded v0**: solo colori e metriche, niente atlanti grafici
  completi — vedi [docs/troubleshooting.md](docs/troubleshooting.md) per
  usare il proprio `.msstyles`.
- **Icona di rete**: compare dal **secondo** avvio della shell (il
  componente che la abilita si scarica in background al primo).
- **Jump list UWP**: le app dello Store mostrano un fallback
  (`AppsFolder`); **Impostazioni non ha jump list per design**.
- **Pagina "Icone area di notifica"** di Windows: su 24H2 esiste ancora ma
  si apre **vuota**; usare la finestra integrata (`NotifyIconsUseSettings=3`).
- **HiDPI**: il layout della taskbar Win7 non è DPI-aware come quello di
  Win11 (limitazione notissima dell'explorer Win7, non risolvibile qui).
- Multi-monitor: la taskbar Win7 esiste solo sul monitor primario
  (limitazione dell'explorer Win7).

## Quick start

1. Scarica **`ex7-test-bundle.zip`** dalla [release di riferimento](https://github.com/babamohammed2022/7explorer/releases)
   e decomprimila in una cartella, ad es. `C:\ex7test`.
2. Avvia **`ex7selfcontained.exe`** dalla cartella (serve Internet la
   prima volta): scarica l'`explorer.exe` di Windows 7 dal symbol server
   Microsoft (con verifica SHA-256), lo patcha verso `wrp64.dll` e genera
   le risorse UI. Alla fine hai `explorer.exe` privato affianco ai
   binari del bundle.
3. Avvia **`7explorer-shell-switcher.exe`**: trova l'explorer privato da
   solo (stessa cartella). Seleziona **Windows 7 Explorer → Cambia** → la
   taskbar di Windows 7 sostituisce quella di Windows 11.
4. Per tornare indietro: **Ctrl+Alt+Shift+S** → *Esplora risorse Windows
   nativo* → **Cambia** (nessun logout).
5. (Opzionale) spunta **"Avvia Explorer7 automaticamente al logon"** per
   renderlo permanente — [docs/avvio-al-login.md](docs/avvio-al-login.md).

Dettagli, incluso l'avvio offline e la tabella di trasparenza dei download:
[docs/installazione.md](docs/installazione.md).

<details>
<summary>Screenshot</summary>

<p align=center>
  <img src="https://github.com/user-attachments/assets/a428c168-b1ca-49cc-aac0-7959cd892cba">
  <br><i>Il menu Start nella vista predefinita.</i>
  <br>
  <img src="https://github.com/user-attachments/assets/edd254ba-1763-415a-b61b-78bd196b1500">
  <br><i>Il menu Start nella vista programmi.</i>
  <br>
  <img src="https://github.com/user-attachments/assets/22320c33-6448-44b5-8ff1-2681fad74b25">
  <br><i>Jump list della taskbar e overflow del tray.</i>
  <br>
</p>

</details>

## Requisiti

- Windows 11 **24H2 / 25H2**, **x64** (target attivo del progetto; 8.1/10
  funzionano in larga parte ma non sono il focus).
- Connessione Internet **solo al primo avvio** (download dell'explorer
  Win7 dal symbol server Microsoft, ~2,8 MB; poi tutto è in cache locale).
- Nessuna elevazione: tutto gira per-utente, niente file di sistema
  toccati.

## Avvio automatico al logon

La casella *"Avvia Explorer7 automaticamente al logon"* dello switcher
imposta il **valore `Shell` per-utente** (`HKCU`): il metodo standard di
Windows per la shell di un utente, **senza elevazione e reversibile**
(valore precedente salvato e ripristinato byte per byte). In più: un link
di fallback nella cartella Esecuzione automatica e un **task di recovery**
che al logon successivo verifica che la shell privata sia partita e, in
caso contrario, ripristina tutto da solo. Documentazione completa, incluse
le procedure di disattivazione e recovery:
**[docs/avvio-al-login.md](docs/avvio-al-login.md)**.

## Documentazione

| documento | contenuto |
|---|---|
| [docs/installazione.md](docs/installazione.md) | procedura completa: bundle, primo avvio (cosa scarica e dove va in cache), avvio offline, tabella di trasparenza URL + SHA-256 |
| [docs/avvio-al-login.md](docs/avvio-al-login.md) | avvio automatico al logon: cosa modifica, come disattivarlo, recovery |
| [docs/opzioni.md](docs/opzioni.md) | tutte le opzioni di registro e `config.ini` (nome, tipo, default, significato) |
| [docs/troubleshooting.md](docs/troubleshooting.md) | dove stanno i log e i problemi noti con le relative soluzioni |
| [docs/config.ini.example](docs/config.ini.example) | esempio commentato del file di configurazione tema |
| [docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md](docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md) | piano tecnico dell'installer self-contained |
| [docs/REPO_CLEANUP.md](docs/REPO_CLEANUP.md) | cronologia della riorganizzazione del repository (branch, release, artefatti) |
| [installer/ex7selfcontained/README.md](installer/ex7selfcontained/README.md) | dettagli tecnici dell'installer |
| [switcher/README.md](switcher/README.md) | dettagli tecnici dello switcher |
| [windhawk/README.md](windhawk/README.md) | mod Windhawk opzionali (sorgente) |

## Componenti del repository

| componente | ruolo |
|---|---|
| `explorerwrapper/` | **`wrp64.dll`**: la wrapper library caricata dall'explorer Win7 privato (import patchati). Hook di sistema, fix, tema, rete, tray. C++ con `/EH` off, CRT minima, codice pericoloso sotto SEH, RAII da `SafeGuards.h`. |
| `installer/ex7selfcontained/` | **bootstrap self-contained**: scarica `explorer.exe` Win7 SP1 x64 dal symbol server Microsoft (SHA-256 fissato nel codice), patcha gli import verso `wrp64.dll`, inietta le risorse UI generate (en-US + it-IT, nessun `.mui`). |
| `switcher/` | **`7explorer-shell-switcher.exe`**: GUI Win32 (CRT statica) che cambia shell a runtime; hotkey globale; avvio al logon. |
| `windhawk/` | 2 mod Windhawk opzionali (sorgente): `ex7-fake-explorer-path` (ora ridondante: lo spoof è integrato in `wrp64.dll`) e `ex7-userinit-shell` (soppiantato dal valore `Shell` per-utente). |
| `localization/`, `tools/`, `tests/` | pipeline di localizzazione: cataloghi JSON → `tools/build_resources.py` → blob validati → `embed_catalog.py`; test Python (51). |
| `ci/`, `.github/workflows/` | CI: `msbuild.yml`, `selfcontained-ci.yml`, `localization-pipeline.yml` + job diagnostici `diag-*.yml` (log come artifact). |

## Sviluppo e build

```
git clone https://github.com/babamohammed2022/7explorer.git
cd 7explorer
git clone https://github.com/TsudaKageyu/minhook.git minhook
msbuild minhook\build\VC17\MinHookVC17.sln /p:Configuration=Release /p:Platform=x64
copy /Y minhook\build\VC17\lib\Release\libMinHook.x64.lib explorerwrapper\
msbuild explorerwrapper.sln /p:Configuration=Release /p:Platform=x64
msbuild switcher\shell_switcher.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild installer\ex7selfcontained\ex7selfcontained.vcxproj /p:Configuration=Release /p:Platform=x64
```

- `libMinHook.x64.lib` **non è nel repository** (è un output di build): va
  compilato dai sorgenti come sopra — la CI lo fa a ogni run.
- Test: `python tests/run_tests.py` (51 test, girano anche su Linux).
- CI su ogni push: build completa + verifica su file Microsoft reale +
  test Python; i log di build sono artifact della run
  (`docs/REPO_CLEANUP.md` spiega perché non sono più su branch).

## Convenzione linguistica

- **Documentazione per gli utenti** (README, `docs/*`): **italiano**,
  lingua principale del progetto e del suo pubblico.
- **Codice, commenti, commit e documentazione tecnica interna**
  (es. `installer/ex7selfcontained/README.md`, messaggi di log):
  **inglese**.
- Le stringhe UI generate: en-US (fallback) + it-IT
  (`localization/catalog/`).

## Licenza e crediti

Codice licenziato **GPLv3** ([LICENSE](LICENSE)). Questo progetto è un
fork di **explorer7** del [World Windows Federation]
(https://github.com/world-windows-federation/explorer7): senza il loro
lavoro niente di tutto questo esisterebbe.

Tecniche e codice derivati, con crediti espliciti anche nei sorgenti:

- **[world-windows-federation/explorer7](https://github.com/world-windows-federation/explorer7)** —
  upstream: wrapper library, meccanismo di import patching, temi, orb.
- **[aubymori — Aero Flyout Fix](https://github.com/aubymori)** — bordi
  Aero sui flyout legacy (`explorerwrapper/FlyoutFrames.cpp`).
- **[valinet/ExplorerPatcher](https://github.com/valinet/ExplorerPatcher)** —
  tecnica dei menu tray classici su SndVolSSO/pnidui
  (`explorerwrapper/ImmersiveMenus.*`), componenti pnidui 22621 per
  l'icona di rete (`explorerwrapper/NetworkIcon.cpp`).

Come da licenza: siete liberi di studiare, modificare e ridistribuire,
sempre con licenza GPLv3 e crediti visibili. L'inclusione dei binari
compilati in ISO Windows modificate o "transformation pack" è sconsigliata.
