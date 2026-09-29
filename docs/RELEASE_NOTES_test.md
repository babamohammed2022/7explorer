# 7explorer — build di riferimento (fase di test)

Questa è la **release di riferimento** del progetto: un'unica build che
corrisponde **esattamente** al tag (`v0.3-test38`) ed è prodotta dalla CI
(GitHub Actions, `windows-latest`, MSVC) dallo stesso commit del tag.
Hash di tutti gli asset in `SHA256SUMS.txt`.

> **Sostituisce tutte le release precedenti** (`v0.0.1-test1` …
> `v0.3-test37`), eliminate insieme ai relativi tag: erano istantanee
> di test sovrapposte; la loro storia resta nei commit. Riepilogo della
> riorganizzazione: `docs/REPO_CLEANUP.md`.

## Download

| file | cosa è |
|---|---|
| **`Win7ExplorerRestorer-test-bundle.zip`** | **il bundle completo** consigliato: `wrp64.dll` + `Win7ExplorerRestorer.exe` + `7explorer-shell-switcher.exe` + README (+ sorgenti dei mod Windhawk opzionali). Decomprimi in una cartella e segui il [quick start](../../#quick-start) |
| `wrp64.dll` | il wrapper (Release x64) — se preferisci i file singoli |
| `Win7ExplorerRestorer.exe` | installer/bootstrap self-contained (Release x64) |
| `7explorer-shell-switcher.exe` | switcher shell runtime + avvio al logon (CRT statica) |
| `SHA256SUMS.txt` | SHA-256 di tutti gli asset |

Documentazione: [docs/installazione.md](../../tree/main/docs/installazione.md)
· [docs/avvio-al-login.md](../../tree/main/docs/avvio-al-login.md) ·
[docs/troubleshooting.md](../../tree/main/docs/troubleshooting.md) ·
[docs/opzioni.md](../../tree/main/docs/opzioni.md).

## Cosa funziona (verificato in sviluppo)

- La shell di Windows 7 gira **come shell di Windows 11 24H2/25H2**:
  taskbar, menu Start, tray, Esplora risorse.
- **Cambio shell istantaneo** Win7 ⇄ Win11 senza logout
  (`Ctrl+Alt+Shift+S` → Usa …).
- **Avvio automatico al logon** (test37): valore `Shell`
  per-utente, reversibile e con backup byte-per-byte, link di fallback
  robusto e task di recovery automatico ~30 s dopo il logon.
- **Switcher UI unificata** (novità test38): vista Setup
  (installa/reinstalla con un clic, barra di avanzamento, dettagli dal
  log) + vista Main (scelta shell, Maggiori informazioni,
  reinstalla/disinstalla, lingua della sola shell Win7 persistita
  per-utente); DPI-aware, Enter/Esc come in un dialog.
- Installer self-contained: download dell'`explorer.exe` Win7 SP1 dal
  symbol server Microsoft con verifica a più livelli, patch import,
  localizzazione **senza `.mui`** (risorse UI en-US + it-IT generate dal
  progetto); offline dal secondo avvio.
- Menu tray classici (volume, rete), flyout batteria 8.1, bordi Aero sui
  flyout (credit aubymori), Win+I, jump list Win32, tema embedded di
  fallback + supporto al tuo `.msstyles`.

## Problemi noti (onesti)

- **Avvio al logon**: la matrice di test completa (logoff/logon, riavvio,
  file rimosso, disattivazione) è **in verifica su hardware reale**
  (test37); il meccanismo di recovery automatico è previsto per
  l'eventuale file mancante. Log: `%TEMP%\7explorer-switcher.log`.
- **Switcher UI unificata** (test38): in questa build è verificata solo
  staticamente — feedback benvenuto su layout/DPI e sui flussi
  installa/reinstalla/disinstalla.
- Icona di rete dal **secondo** avvio (download in background al primo).
- Tema embedded **v0**: solo colori/metriche — usa il tuo `aero.msstyles`
  per il look completo.
- Pagina "Icone area di notifica" di sistema **vuota su 24H2** →
  `NotifyIconsUseSettings=3` (finestra integrata).
- Jump list UWP: fallback `AppsFolder` (test36); **Impostazioni non ha
  jump list per design**.
- HiDPI parziale (limitazione explorer Win7); taskbar solo sul monitor
  primario.

## File scaricati a runtime (trasparenza)

Tutti gli URL e gli SHA-256 sono **fissati nel codice**; un file con hash
imprevisto viene scartato. Nessun binario Microsoft è contenuto negli
asset di questa release.

| file | URL | SHA-256 |
|---|---|---|
| `explorer.exe` Win7 SP1 x64 (6.1.7601.17514) — all'installazione | `https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe` | `5769e5b25c7bfbc20dbfdca2f17b751f6d968e03412705de4a16c99b2626e21b` / `6a671b92a69755de6fd063fcbe4ba926d83b49f78c42dbaeed8cdb6bbc57576a` (allow-list, due copie legittime) |
| `pnidui.dll` 10.0.22621.3810 — primo avvio shell (rete) | `https://msdl.microsoft.com/download/symbols/pnidui.dll/F717CABC20B000/pnidui.dll` | `52e9c88cb50ef98e683839ab62880180f0dd3db3e1c445ea461ae63d97a349d3` |
| `pnidui.dll.mui` en-US / it-IT — primo avvio shell (rete) | `https://raw.githubusercontent.com/valinet/ExplorerPatcher/0a88a6e0ef6b1752fea36e581cffff1097e862b0/ep_setup/resources/files/pnidui/<lang>/pnidui.dll.mui` | en-US `ae0d2655cb9806b5c16b92f82d698f5c98feaef1f78ebdd6014dccc8304184ed` · it-IT `e7ce6e6e43483815b79946b05e6f744e9277a123ef387485826d558533609f20` |
| `batmeter.dll` 6.3.9600.17415 — primo avvio shell (batteria, solo se presente) | `https://msdl.microsoft.com/download/symbols/batmeter.dll/545054931f3000/batmeter.dll` | `f32f18d44f9a6511c73ca1a9a4a6edad38aff23a15fd4c75d9aaaaf31526a506` |
| `stobject.dll` 6.3.9600.17415 — primo avvio shell (batteria, solo se presente) | `https://msdl.microsoft.com/download/symbols/stobject.dll/54503A4356000/stobject.dll` | `30737741f7131ff80706c7d12b2fe8ab8a6203aeaf9984d9a7c908c0f2565149` |

(Le ~30 lingue dei `.mui` di pnidui sono tabellate in
`explorerwrapper/NetworkIcon.cpp`.)

## Requisiti

Windows 11 24H2/25H2 x64, connessione solo al primo avvio, nessuna
elevazione. Licenza **GPLv3** — crediti: upstream
[explorer7](https://github.com/world-windows-federation/explorer7),
aubymori (Aero flyout fix), valinet/ExplorerPatcher (tray classici,
pnidui).
