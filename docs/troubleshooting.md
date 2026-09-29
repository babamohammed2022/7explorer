# Troubleshooting e problemi noti

## Dove stanno i log

| log | chi lo scrive | contenuto |
|---|---|---|
| `%TEMP%\7explorer-shellfix.log` | `wrp64.dll` (la shell) | avvio della shell, hook installati, tray, rete, jump list, guardia anti-iniezione. Cap 256 KB con rotazione. Disattivabile con `ShellFixLog=0`. |
| `%TEMP%\7explorer-switcher.log` | `shell-switcher.exe` | switch runtime, avvio automatico al logon (registro, link, task), recovery. Indispensabile per diagnosticare il logon. |
| `%LocalAppData%\7explorer\theme.log` | tema (ThemeManager) | caricamento/estrazione del tema, fallback. |
| `<cartella bundle>\log\Win7ExplorerRestorerSetup.log` | `Win7ExplorerRestorer.exe` | download, verifica hash, patch, risorse. |

In più, tutto viene inviato a `OutputDebugString`: con
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
si vede in tempo reale (filtra per `[Win7ExplorerRestorer]`).

Allega **questi log** quando segnali un problema.

## Problemi noti e soluzioni

### Icona di rete assente al primo avvio

**Comportamento**: alla prima esecuzione della shell Win7 l'icona di rete
non compare; compare dal secondo avvio.

**Perché**: il componente che la abilita (`pnidui.dll` 22621 + `.mui`)
viene scaricato **in background** durante il primo avvio (URL e SHA-256
fissati nel codice, vedi [installazione.md](installazione.md)); solo
dopo un download verificato l'icona può essere creata. Nel log
(`%TEMP%\7explorer-shellfix.log`): `cache ...: incomplete (icon from the
next start after a successful download)`.

**Soluzione**: nessuna, è by-design — riavvia la shell una seconda volta
(`Ctrl+Alt+Shift+S` → Cambia). Se dopo il secondo avvio manca ancora,
controlla nel log le righe `[Win7ExplorerRestorer][net]` (download fallito? hash?) e che
`%LocalAppData%\7explorer\pnidui-F717CABC20B000\pnidui.dll` esista.

### Tema "embedded" diverso dall'Aero originale

**Comportamento**: con il tema embedded la taskbar ha i colori/le metriche
Win7-like ma non è il facsimile grafico completo.

**Perché**: il tema embedded è **v0: solo colori e metriche, nessun
atlante grafico** — dichiarato dal generatore stesso
(`tools/theme/build_theme.py`: *"v0 scope: structural probe themes
(colors only, no atlases)"*). Il facsimile completo è lavoro in corso.

**Soluzione**: usa il **tuo** file `.msstyles` di Windows 7:

1. pulsante **Tema…** nello switcher: seleziona il tuo `aero.msstyles`
   (e gli eventuali `en-US\`/`it-IT\aero.msstyles.mui` accanto); viene
   copiato in `<cartella explorer.exe>\theme\`;
2. oppure copia manualmente il file in `<cartella explorer.exe>\theme\`
   come `aero.msstyles`;
3. configura `config.ini` se vuoi un nome/modo specifico
   ([config.ini.example](config.ini.example), [opzioni.md](opzioni.md)).

Il file resta **tuo**: viene solo copiato in locale, mai inviato da
nessuna parte. Cambia shell per applicarlo. Diagnostica in
`%LocalAppData%\7explorer\theme.log`.

### Pagina "Icone area di notifica" vuota su 24H2

**Comportamento**: "Personalizza" / "Icone area di notifica" apre la
pagina di sistema che su 24H2 esiste ancora ma appare **vuota**.

**Perché** *(ipotesi, non accertata)*: la pagina
`::{05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9}` esiste ancora su 24H2/25H2
(verificato in test31: il CLSID è registrato e la finestra si apre), ma il
suo contenuto su 24H2 non viene popolato — probabilmente dipende da
componenti di Impostazioni moderni agganciati alla shell Win11. Si tratta
di un'ipotesi sul *perché* della pagina vuota, non di un fatto verificato:
il fatto verificato è che la pagina si apre e resta vuota.

**Soluzione**: usa la **finestra integrata** del progetto, che riproduce
la pagina (elenco icone, comportamento per icona, "mostra sempre tutte"):

```
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v NotifyIconsUseSettings /t REG_DWORD /d 3 /f
```

poi riavvia la shell. Con `NotifyIconsUseSettings=0` (default) la scelta è
automatica. Tutti i valori possibili in [opzioni.md](opzioni.md).

### Jump list delle app UWP / Impostazioni

**Stato onesto (test36)**:

- **Impostazioni non ha jump list per design**: nel codice il resolver non
  viene nemmeno interrogato per `ms-settings` (non è un bug).
- **Altre app UWP** (Store): il resolver Win8+ non trova un file `.lnk`
  per gli AUMID, quindi il progetto fornisce in fallback l'elemento
  `shell:AppsFolder\<AUMID>` (lo stesso che usa la taskbar moderna).
  Attivo di default (`UwpJumpLists=1`).
- **Se una jump list UWP non appare**: cerca `[Win7ExplorerRestorer][jumplist]` in
  `%TEMP%\7explorer-shellfix.log` — ogni risoluzione logga il risultato
  del resolver e del fallback (`resolver 0x…, AppsFolder fallback 0x…`).
  Se il fallback è `0x…` ≠ 0, l'app non espone un elemento AppsFolder
  utilizzabile: segnala l'AUMID esatto (dal log) in una issue.

### "Nessuna shell" (schermo nero / solo sfondo)

Se dopo uno switch non c'è nessuna barra:

1. **`Ctrl+Alt+Shift+S`** → apre lo switcher → seleziona una shell →
   **Cambia** (funziona anche senza shell: l'istanza resident è
   indipendente da explorer);
2. altrimenti `Ctrl+Shift+Esc` → Gestione attività → **Esegui nuova
   attività** → `explorer.exe` → OK (riparte la shell nativa di Windows);
3. con l'avvio automatico al logon attivo, il **task di recovery**
   interviene da solo ~30 s dopo il logon
   ([avvio-al-login.md](avvio-al-login.md#recovery-se-la-shell-privata-non-parte)).

### L'avvio automatico al logon non funziona

Vedi [avvio-al-login.md](avvio-al-login.md). Primo strumento: il log
`%TEMP%\7explorer-switcher.log`, che registra ogni fase (valore Shell
scritto/ripristinato, link, task di recovery, switch verificati,
`--recover-login`).

### Menu/aprire file con caratteri errati (lingua)

La shell privata usa en-US (fallback) o it-IT a seconda della lingua del
sistema. Lo switcher permette di forzare la lingua UI per gli avvii da
esso gestiti (combo "Lingua UI di Windows 7 Explorer Restorer"); per l'avvio da logon vale
la variabile d'ambiente `WIN7EXPLORERRESTORER_UI_LANG` utente (vedi
[avvio-al-login.md](avvio-al-login.md#dettagli-tecnici-e-limiti-noti)).

### Mod Windhawk che non si carica

La shell privata ha una **guardia anti-iniezione**: i mod che hanno
crashato l'avvio finiscono in quarantena (`InjectionQuarantine`) e i mod
non fidati possono essere bloccati in safe mode o con policy ≥2. Nel log:
`[Win7ExplorerRestorer] injection guard: ...`. Policy, allow-list e quarantena sono
documentati in [opzioni.md](opzioni.md#guardia-anti-iniezione-windhawk).
