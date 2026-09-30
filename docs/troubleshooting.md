# Troubleshooting and known issues

## Log locations

| Log | Written by | Contents |
|---|---|---|
| `%TEMP%\7explorer-shellfix.log` | `wrp64.dll` (the shell) | Shell startup, installed hooks, notification area, network, jump lists, and injection guard. 256 KB limit with rotation. Disable with `ShellFixLog=0`. |
| `%TEMP%\7explorer-switcher.log` | `shell-switcher.exe` | Runtime shell switches, automatic logon startup (registry, shortcut, task), and recovery. Essential for diagnosing logon issues. |
| `%LocalAppData%\7explorer\theme.log` | Theme manager (`ThemeManager`) | Theme loading/extraction and fallback. |
| `<bundle folder>\log\Win7ExplorerRestorerSetup.log` | `Win7ExplorerRestorer.exe` | Downloads, hash checks, patching, and resources. |

Everything is also sent to `OutputDebugString`. Use [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) to view it in real time (filter for `[Win7ExplorerRestorer]`).

Attach **these logs** when reporting an issue.

## Known issues and workarounds

### AutoPlay does not appear when you connect a volume

The private shell listens for volume-arrival events (removable drives, USB disks exposed as fixed drives, and CD/DVDs), then asks Windows to invoke the `autoplay` verb registered for the drive letter. This is **best-effort** support, not a guarantee that the native AutoPlay window will appear: Windows may not expose the verb or may suppress it based on the build, AutoPlay settings, system policies, or installed handlers. MTP devices that do not expose a volume are outside the basic supported case.

First check **Settings → Bluetooth & devices → AutoPlay** and the user/system AutoPlay policies. Disable the listener with `AutoPlayDeviceNotifications=0` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced`; `FixAutoPlay=0` disables only the per-user policy repair. For diagnosis, search for `AutoPlay monitor` in `%TEMP%\7explorer-shellfix.log` or capture `OutputDebugString` with DebugView.

To restore the classic Windows 7 dialog, the separate Windhawk mod [Windows 7 Classic AutoPlay Dialog Restorer](https://windhawk.net/mods/win7-classic-autoplay-restorer) (`win7-classic-autoplay-restorer`) is recommended. Install it through Windhawk; it is not included with this project.

### Taskbar pinning does not work on Windows 11

Testing of release [`v0.1.0-alpha-test.1`](https://github.com/babamohammed2022/Windows7ExplorerRestorer/releases/tag/v0.1.0-alpha-test.1) confirmed that pinning from the Windows 7 taskbar **does not work**. The code contains compatibility attempts using native interfaces and an internal Win32 taskbar path, but these are experimental and do not solve the problem yet. `UseTaskbarPinning=1` enables the attempts; it does not guarantee pinning. `UseTaskbarPinning=0` disables pins and is not a workaround. For now, consider this feature unsupported on Windows 11.

### Network icon missing on first startup

**What happens**: the network icon does not appear the first time the Win7 shell runs; it appears after the second startup.

**Why**: the component that enables it (`pnidui.dll` 22621 + `.mui`) is downloaded **in the background** during the first startup (the URL and SHA-256 are pinned in the source; see [installazione.md](installazione.md)). The icon can be created only after a successful download. The log may contain `cache ...: incomplete (icon from the next start after a successful download)` in `%TEMP%\7explorer-shellfix.log`.

**What to do**: nothing; this is expected. Restart the shell a second time (`Ctrl+Alt+Shift+S` → **Switch**). If the icon is still missing after the second startup, check the `[Win7ExplorerRestorer][net]` entries in the log (download failure? hash mismatch?) and verify that `%LocalAppData%\7explorer\pnidui-F717CABC20B000\pnidui.dll` exists.

### Embedded theme differs from the original Aero theme

**What happens**: with the embedded theme, the taskbar has Win7-like colors and metrics, but is not a complete visual replica.

**Why**: the embedded theme is **v0: colors and metrics only, with no graphics atlas**, as stated by its generator (`tools/theme/build_theme.py`: *"v0 scope: structural probe themes (colors only, no atlases)"*). A complete replica is still in progress.

**What to do**: use **your own** Windows 7 `.msstyles` file:

1. In the switcher, click **Theme…** and select your `aero.msstyles` (and any `en-US\`/`it-IT\aero.msstyles.mui` files alongside it); the files are copied to `<explorer.exe folder>\theme\`.
2. Or copy the file manually to `<explorer.exe folder>\theme\` as `aero.msstyles`.
3. Configure `config.ini` if you want a specific theme name or mode ([config.ini.example](config.ini.example), [opzioni.md](opzioni.md)).

The file remains **yours**: it is copied locally and never sent anywhere. Switch shells to apply it. Diagnostics are in `%LocalAppData%\7explorer\theme.log`.

### Notification Area Icons page is blank on 24H2

**What happens**: **Customize** / **Notification Area Icons** opens the system page, which still exists on 24H2 but appears **blank**.

**Why** *(hypothesis, not confirmed)*: the page `::{05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9}` still exists on 24H2/25H2 (verified in test31: the CLSID is registered and the window opens), but its contents are not populated on 24H2—possibly because it depends on modern Settings components attached to the Windows 11 shell. This explanation is a hypothesis, not a verified fact. The verified fact is that the page opens and remains blank.

**Workaround**: use the project's **built-in window**, which recreates the page (icon list, per-icon behavior, and **Always show all icons**):

```
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v NotifyIconsUseSettings /t REG_DWORD /d 3 /f
```

Then restart the shell. With `NotifyIconsUseSettings=0` (the default), the choice is automatic. See [opzioni.md](opzioni.md) for all available values.

### UWP app / Settings jump lists

**Current behavior (test36)**:

- **Settings has no jump list by design**: the resolver is not queried for `ms-settings` (this is not a bug).
- **Other UWP (Store) apps**: the Win8+ resolver cannot find an `.lnk` for their AUMIDs, so the project supplies `shell:AppsFolder\<AUMID>` as a fallback item (the same item used by the modern taskbar). Enabled by default (`UwpJumpLists=1`).
- **If a UWP jump list does not appear**: search for `[Win7ExplorerRestorer][jumplist]` in `%TEMP%\7explorer-shellfix.log`. Each resolution logs the resolver and fallback results (`resolver 0x…, AppsFolder fallback 0x…`). If the fallback is `0x…` ≠ 0, the app does not expose a usable AppsFolder item; report the exact AUMID from the log in an issue.

### "No shell" (black screen / wallpaper only)

If no taskbar appears after switching:

1. Press **`Ctrl+Alt+Shift+S`** → open the switcher → select a shell → **Switch** (works even without a shell; the resident instance runs independently of Explorer).
2. Otherwise, press `Ctrl+Shift+Esc` → Task Manager → **Run new task** → enter `explorer.exe` → OK (restarts the native Windows shell).
3. If automatic logon startup is enabled, the **recovery task** runs by itself about 30 seconds after logon ([avvio-al-login.md](avvio-al-login.md#recovery-if-the-private-shell-does-not-start)).

### Automatic logon startup does not work

See [avvio-al-login.md](avvio-al-login.md). The first diagnostic tool is `%TEMP%\7explorer-switcher.log`, which records every stage (the `Shell` value written/restored, shortcut, recovery task, verified switches, and `--recover-login`).

### Incorrect characters in menus or file names (language)

The private shell uses en-US (fallback) or it-IT, depending on the system language. The switcher can force the UI language for launches it manages (the **Windows 7 Explorer Restorer UI language** drop-down). For logon startup, use the per-user `WIN7EXPLORERRESTORER_UI_LANG` environment variable (see [avvio-al-login.md](avvio-al-login.md#technical-details-and-known-limitations)).

### Windhawk mod does not load

The private shell has an **injection guard**: mods that crashed during startup are quarantined (`InjectionQuarantine`), and untrusted mods may be blocked in safe mode or with policy ≥2. Check the log for `[Win7ExplorerRestorer] injection guard: ...`. Policies, the allow-list, and the quarantine are documented in [opzioni.md](opzioni.md#windhawk-injection-guard).
