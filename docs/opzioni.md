# Opzioni di configurazione

Tutte le opzioni della shell (wrp64.dll) sono valori di registro sotto:

```
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced
```

sono **per-utente** (HKCU), si applicano all'avvio successivo della shell
privata e nessuna richiede elevazione. La colonna "default" è il valore
usato quando l'opzione non esiste (verificata nel codice con
`ReadAdvancedDword`/`ReadAdvancedDwordPublic` in `explorerwrapper/`).

Indice: [shell e avvio](#shell-e-avvio) · [scorciatoie e log](#scorciatoie-e-log)
· [UWP / app moderne](#uwp--app-moderne) · [tray, menu e flyout](#tray-menu-e-flyout)
· [rete](#rete) · [batteria](#batteria) · [guardia anti-iniezione windhawk](#guardia-anti-iniezione-windhawk)
· [tema e aspetto upstream](#tema-e-aspetto-upstream) · [configini](#configini-theme)
· [valori di stato scritti-dal-codice](#valori-di-stato-scritti-dal-codice)

---

## Shell e avvio

| nome | tipo | default | significato |
|---|---|---|---|
| `ForceShell` | DWORD | 1 | Hook di `ShouldStartDesktopAndTray` (explorer Win7 6.1.7601.17514, verificato a byte prima dell'hook): la risposta è sempre TRUE, così l'explorer non può rifiutarsi di creare desktop e taskbar. Logga anche `CreateDesktopAndTray`. **Disattivando**: l'explorer può decidere da solo di non creare il desktop (comportamento originale) — ad es. se un altro desktop è già presente. |
| `ForceExplorerIsShell` | DWORD | 1 | Hook di `GetPrivateProfileStringW("boot","shell",...,system.ini)`: quando il valore `Shell` (HKCU prima, poi HKLM) nomina un altro programma, l'explorer Win7 uscirebbe come "finestra cartella" (test23). L'hook risponde con il nome del proprio eseguibile e lascia decidere agli altri controlli. **Disattivando**: con l'avvio al logon attivo (valore `Shell` per-utente) l'explorer esce con codice 1 → schermo nero. Non disattivarlo insieme all'avvio automatico. |

## Scorciatoie e log

| nome | tipo | default | significato |
|---|---|---|---|
| `SwitcherHotkey` | DWORD | 1 | Quando la shell privata parte, `wrp64.dll` avvia `shell-switcher.exe --hotkey` (istanza resident che possiede **Ctrl+Alt+Shift+S**), se lo trova accanto a `explorer.exe` o `wrp64.dll`. **Disattivando**: dopo il logon la scorciatoia non è attiva finché non apri la GUI dello switcher. |
| `SettingsHotkey` | DWORD | 1 | Possiede **Win+I** (con fallback a hook tastiera a basso livello) e apre Impostazioni con lo stesso percorso dei remap (`ms-settings:` → shell Win32). **Disattivando**: Win+I non fa nulla nella shell Win7. |
| `ShellFixLog` | DWORD | 1 | Logging della shell su `%TEMP%\7explorer-shellfix.log` (cap 256 KB) + `OutputDebugString`. **Disattivando**: nessun log file (OutputDebugString resta). |

## UWP / app moderne

| nome | tipo | default | significato |
|---|---|---|---|
| `EnableImmersive` | DWORD | 0 | Opzione upstream: abilita lo stack immersive/UWP nella shell Win7 (app dello Store nel menu Start/taskbar, flyout DComp). `StoreAppsInStart`, `StoreAppsOnTaskbar` e `UseDCompFlyouts` hanno effetto solo se questa è 1. **Disattivando**: le app UWP non si avviano dalla shell Win7 (comportamento pre-Win8). |
| `UwpHostRuntime` | DWORD | 1 | Host `ShellAppRuntime.exe` per l'attivazione UWP: **1** = automatico (l'host parte solo se il TwinUI in-process non è in esecuzione); **2** = host sempre avviato all'avvio della shell; **3** = host avviato **prima** del desktop Win7 (ordine segnalato come funzionante sui forum, ma può rubare le icone tray — test34 non lo usa in automatico); **0** = mai. |
| `UwpActivationShim` | DWORD | 1 | Attivazione delle app UWP via `IApplicationActivationManager` con shim (permessi foreground ecc.). **Disattivando**: l'attivazione torna al percorso originale (spesso fallisce senza la shell moderna). |
| `UwpJumpLists` | DWORD | 1 | Jump list delle app UWP: quando il resolver Win8+ non trova un `.lnk` (app dello Store), viene fornito in fallback l'elemento `shell:AppsFolder\<AUMID>` (test36). **Disattivando**: niente jump list per le app UWP (comportamento pre-test36). Nota: **Impostazioni non ha jump list per design** (nel codice il resolver non viene nemmeno interrogato per `ms-settings`). |
| `UwpEarlyHost` | DWORD | 2 | **Legacy/ignorato**: un tempo impostato automaticamente quando l'host tardivo non bastava (avvio anticipato al logon successivo). Da test34 il valore è ancora letto ma non più usato né scritto in automatico; per l'avvio anticipato manuale usare `UwpHostRuntime=3`. |
| `SettingsWin32Remap` | DWORD | 1 | Rimappa gli URL `ms-settings:` verso le finestre/classici Win32 equivalenti (Taskbar → proprietà taskbar Win7, ecc.). **Disattivando**: `ms-settings:` passa al percorso moderno (che senza shell Win11 fallisce). |
| `ImmersiveInitFailures` | DWORD | 0 *(stato)* | Sentinella scritta dal codice: conta gli avvii in cui l'init UWP è iniziato ma non è mai riuscito. Dopo **2** di fila UWP resta disattivato finché il valore non viene azzerato o eliminato. |

## Tray, menu e flyout

| nome | tipo | default | significato |
|---|---|---|---|
| `ClassicTrayMenus` | DWORD | 1 | Menu tray classici disegnati dal proprietario (tecnica ExplorerPatcher) per i moduli tray caricati (volume, rete). **Disattivando**: i menu tornano a quelli del modulo (stile moderno quando presente). |
| `ClassicVolumeFlyout` | DWORD | 1 | Flyout volume classico: imposta `EnableMTCUVC=0` per SndVolSSO così il flyout Win7-style viene usato (test25). **Disattivando**: flyout volume moderno di Windows 11. |
| `VolumeMenuActions` | DWORD | 1 | Voci di azione hardcoded nel menu volume (Apri mixer, Riproduzione dispositivi…), perché le azioni originali non risolvono su 24H2. **Disattivando**: menu volume senza azioni. |
| `AeroFlyoutFrames` | DWORD | 1 | Bordi/lati Aero sui flyout legacy (credit: **aubymori**, "Aero Flyout Fix"). **Disattivando**: flyout senza il frame Aero. |
| `OpaqueThumbnails` | DWORD | 0 | Anteprime taskbar opache con gradiente (comportamento upstream) invece di translucide. **Attivando**: thumbnail opachi. |
| `NotifyIconsUseSettings` | DWORD | 0 | Dove apre "Personalizza icone notifica": **0** = automatico (la pagina di sistema `::{05D7B0F4-…}` "Icone area di notifica" se il suo CLSID è registrato — esiste ancora su 24H2/25H2 ma su 24H2 si apre **vuota** — altrimenti la finestra integrata); **1** = app Impostazioni (`ms-settings:taskbar`); **2** = sempre la pagina di sistema; **3** = sempre la **finestra integrata** (ricreata dal progetto, raccomandata su 24H2). |
| `FixHelpAndSupportName` | DWORD | 1 | Fix del nome visualizzato "Guida e supporto" (Win7 it-IT legge il nome dal .mui che qui non esiste). **Disattivando**: possibile nome/etichetta errata nel menu Start. |
| `FixConnectTo` | DWORD | 1 | Registra il CLSID "Connetti a" (`{38A98528-…}`, verb + TreatAs) solo se il sistema non ne ha uno; apre la sezione Video (`shell:::{18989B1D-…}`). **Disattivando**: "Connetti a" può non funzionare. |
| `FixAutoPlay` | DWORD | 1 | Ripara i criteri AutoPlay per-utente che lo disabilitano del tutto (HKCU-only, test39): `NoDriveTypeAutoRun=0xFF` → `0x91` e `NoAutoplayfornonVolume≠0` → `0` (default Wine, come il mod `win7-classic-autoplay-restorer`); valori mancanti lasciati stare, HKLM mai toccato. **Disattivando**: nessuna riparazione. |
| `KeepSystemTransparency` | DWORD | 0 | Per default la shell privata forza la trasparenza DWM attiva (per l'Aero della taskbar). **Attivando**: la shell rispetta l'impostazione di trasparenza di sistema (taskbar opaca se il sistema ha effetti trasparenza off). |

## Rete

| nome | tipo | default | significato |
|---|---|---|---|
| `NetworkIconEngine` | DWORD | 1 | Motore dell'icona di rete (test32): **1** = icona propria guidata dal Network List Manager (`NetworkTrayIcon.cpp`), perché su 24H2 il motore di stato del pnidui 22621 resta congelato; **0** = comportamento precedente (SSO pnidui ospitato da stobject). |
| `LegacyNetworkIcon` | DWORD | 1 | Icona di rete "legacy" via pnidui 22621 scaricato: **1** = automatico (usa il pnidui legacy solo se il sistema non ne ha uno); **2** = forza l'uso di quello legacy anche se presente nel sistema; **0** = disattivata. |
| `NetworkIconSystemGuid` | DWORD | 1 | L'icona di rete è registrata con il GUID dell'**icona di sistema** di rete di Win7 (sempre visibile, anche con "nascondi icone"), invece del GUID SSO (test35). **Disattivando**: l'icona usa il GUID SSO (può risultare nascosta/assente). |
| `StartNetworkIcon` | DWORD | 1 | Preparazione in background dell'icona di rete all'avvio della shell (download + verifica pnidui). **Disattivando**: nessuna icona di rete preparata. |

Nota pratica: al **primo** avvio il componente viene scaricato in
background; l'icona compare dal **secondo** avvio (vedi
[troubleshooting](troubleshooting.md#icona-di-rete-assente-al-primo-avvio)).

## Batteria

| nome | tipo | default | significato |
|---|---|---|---|
| `Win32BatteryFlyout` | DWORD | 1 | Flyout batteria: carica stobject/batmeter 8.1 (download verificato dal symbol server, cache in `%LocalAppData%\7explorer\w81flyout`) con patch IAT `CreateWindowInBand`. **Disattivando**: flyout batteria del sistema. |
| `W81SysTrayWrapper` | DWORD | 1 | L'oggetto 8.1 viene wrappato in `CSysTrayWrapper` (gestione sicura del ciclo di vita). **Disattivando**: l'oggetto viene usato raw. |
| `BatteryFlyoutFallback` | DWORD | 0 | Fallback quando il flyout 8.1 non è disponibile (senza batteria rilevata, build non supportata, download fallito): tooltip con la percentuale. **Attivando**: tooltip percentuale sempre disponibile come rete di sicurezza. |
| `W81FlyoutForce` | DWORD | 0 | 1 = ignora la tabella dei build supportati e prova comunque a caricare i file 8.1. |
| `W81StobjectId`, `W81StobjectSha256`, `W81BatmeterId`, `W81BatmeterSha256` | SZ | — | Override REG_SZ per un build 8.1 alternativo (l'id è `TimeDateStamp %08X` + `SizeOfImage %x`, come nell'URL del symbol server). Servono entrambi (id e sha256) per valore. |

## Guardia anti-iniezione (Windhawk)

| nome | tipo | default | significato |
|---|---|---|---|
| `InjectionGuard` | DWORD | 1 | Hook `LdrLoadDll` + `UnhandledExceptionFilter`: tutto ciò che si inietta nell'explorer privato è classificato e (se necessario) bloccato, sotto SEH. **Disattivando**: qualsiasi DLL può iniettarsi (rischio crash della shell). |
| `InjectionPolicy` | DWORD | 1 | **0** = nessun blocco (solo log); **1** = default: ogni mod carica, solo quelli che hanno **crashato** vengono messi in quarantena; **≥2** = allow-list stretta: i mod Windhawk non in lista vengono bloccati subito. |
| `InjectionSwallow` | DWORD | 1 | I moduli bloccati vengono "inghiottiti": `LoadLibrary` fallisce senza crash del chiamante. **Disattivando**: il modulo si carica davvero (solo se non in quarantena). |
| `InjectionAllowlist` | MULTI_SZ | — | Elenco dei mod Windhawk ammessi (percorso o chiave), uno per stringa. Usata dalla policy ≥2 e dalla safe mode. |
| `InjectionBuiltinAllow` | DWORD | 1 | Allow-list integrata: `win7-network-flyout-recreation` e `win7-action-center-recreation` (scritti per il tray Win7 di questa shell). **Disattivando**: anche questi mod vanno in `InjectionAllowlist`. |

## Tema e aspetto (upstream)

Opzioni ereditate da explorer7, lette da
`explorerwrapper/OptionConfig.cpp`/`RegistryManager.cpp`:

| nome | tipo | default | significato |
|---|---|---|---|
| `Theme` | SZ | `aero` | Nome del tema in `<exedir>\theme\` (relativo; es. `aero` → `theme\aero.msstyles`, `Aero\aero` → `theme\Aero\aero.msstyles`). |
| `OrbDirectory` | SZ | *(interno)* | Directory delle immagini orb in `<exedir>\orbs\` (solo `.bmp`, vedi il README). |
| `DisableComposition` | DWORD | 0 | 1 = la shell si comporta come se DWM non fosse attivo. |
| `ClassicTheme` | DWORD | 0 | 1 = tema Windows Classico. |
| `ColorizationOptions` | DWORD | 1 | Comportamento colorizzazione shell (1–4, compatibilità variabile). |
| `AcrylicColorization` | DWORD | 0 | Colorizzazione acrilica (0–2 colori immersive, 3 = colorizzazione normale). |
| `OverrideAlpha` | DWORD | 0 | 1 = sovrascrive l'alfa della colorizzazione DWM su taskbar/menu/anteprime. |
| `AlphaValue` | DWORD | 0x6B | Valore alfa (2 cifre esadecimali) da usare con `OverrideAlpha=1`. |
| `UseTaskbarPinning` | DWORD | 1 | 0 = niente pin nella taskbar (né caricati né modificabili dalle jump list). |
| `StoreAppsInStart` | DWORD | 1 | 0 = app immersive nascoste dall'elenco "Tutti i programmi" (solo con `EnableImmersive=1`). |
| `StoreAppsOnTaskbar` | DWORD | =`EnableImmersive` | 0 = icone app immersive non applicate/nascoste nei pin (solo con `EnableImmersive=1`). |
| `UseDCompFlyouts` | DWORD | =`EnableImmersive` | Flyout DComp (solo con `EnableImmersive=1`). |

## config.ini [Theme]

Accanto a `explorer.exe` si può mettere un file `config.ini` con la sezione
`[Theme]` (esempio commentato: [docs/config.ini.example](config.ini.example));
selettori gestiti da `explorerwrapper/ThemeManager.cpp`:

| chiave | valori | significato |
|---|---|---|
| `Mode` | `Auto` *(default)* | tema utente da `<exedir>\theme\` se presente, altrimenti tema embedded (auto-estratto in `%LocalAppData%\7explorer\theme\aero.msstyles`). |
| | `Fallback` | sempre il tema embedded, ignora i file esterni. |
| | `Custom` | prova sempre `<exedir>\theme\<Nome>.msstyles`; in caso di errore torna all'embedded. |
| | `Windows7` | alias di `Custom` (per fornire il proprio `aero.msstyles` di Windows 7). |
| | `Windows81` | alias di `Custom` (per fornire un `aero.msstyles` di Windows 8.1). |
| `Name` | nome file *(default `aero`, oppure il valore registry `Theme`)* | nome base del `.msstyles` in `theme\` (senza estensione). |

In tutti i modi, se il caricamento scelto fallisce si ripiega
sull'embedded e, in ultima istanza, sul look classico: l'avvio non è mai
impedito. Diagnostica: `%LocalAppData%\7explorer\theme.log`.

## Valori di stato (scritti dal codice)

Questi valori vengono **scritti** dalla shell per ricordare stati fra un
avvio e l'altro; in genere non vanno impostati a mano:

| nome | significato |
|---|---|
| `StartupFailures` | avvii della shell che non hanno raggiunto la taskbar; a **2** scatta la *safe mode* (mod non in allow-list bloccati). Azzerato a ogni avvio riuscito. Eliminarlo per uscire dalla safe mode. |
| `ImmersiveInitFailures` | vedi tabella UWP. |
| `InjectionQuarantine` | REG_MULTI_SZ: moduli che hanno crashato l'avvio, bloccati ai riavvii successivi. Eliminarlo per riprovare i moduli messi in quarantena. |

---

Le opzioni dello **switcher** (avvio al logon, link, task di recovery) non
stanno qui: usano `HKCU\…\Winlogon\Shell` e la cartella Esecuzione
automatica — vedi [avvio-al-login.md](avvio-al-login.md).
