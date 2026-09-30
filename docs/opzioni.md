# Configuration options

All shell (`wrp64.dll`) options are registry values under:

```
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced
```

They are **per-user** (HKCU), take effect the next time the private shell starts, and never require elevation. The "default" column shows the value used when an option is absent (verified in `ReadAdvancedDword`/`ReadAdvancedDwordPublic` in `explorerwrapper/`).

Contents: [shell and startup](#shell-and-startup) · [shortcuts and logging](#shortcuts-and-logging) · [UWP / modern apps](#uwp--modern-apps) · [notification area, menus, and flyouts](#notification-area-menus-and-flyouts) · [network](#network) · [battery](#battery) · [Windhawk injection guard](#windhawk-injection-guard) · [upstream theme and appearance](#upstream-theme-and-appearance) · [`config.ini` theme](#configini-theme) · [code-written state values](#code-written-state-values)

---

## Shell and startup

| Name | Type | Default | Description |
|---|---|---:|---|
| `ForceShell` | DWORD | 1 | Hook for `ShouldStartDesktopAndTray` (Win7 Explorer 6.1.7601.17514; bytes verified before the hook): always returns TRUE so Explorer cannot refuse to create the desktop and taskbar. Also logs `CreateDesktopAndTray`. **When disabled**: Explorer decides whether to create the desktop itself (original behavior), for example if another desktop already exists. |
| `ForceExplorerIsShell` | DWORD | 1 | Hook for `GetPrivateProfileStringW("boot","shell",...,system.ini)`: when the `Shell` value (HKCU first, then HKLM) names another program, Win7 Explorer would exit as a "folder window" (test23). The hook returns the name of its own executable and lets the other checks decide. **When disabled**: with automatic logon startup enabled (per-user `Shell` value), Explorer exits with code 1 and leaves a black screen. Do not disable this while automatic startup is enabled. |

## Shortcuts and logging

| Name | Type | Default | Description |
|---|---|---:|---|
| `SwitcherHotkey` | DWORD | 1 | When the private shell starts, `wrp64.dll` launches `shell-switcher.exe --hotkey` (a resident instance that owns **Ctrl+Alt+Shift+S**) if it finds it next to `explorer.exe` or `wrp64.dll`. **When disabled**: after logon, the shortcut is inactive until you open the switcher GUI. |
| `SettingsHotkey` | DWORD | 1 | Owns **Win+I** (with a low-level keyboard-hook fallback) and opens Settings through the same remapping path (`ms-settings:` → Win32 shell). **When disabled**: Win+I does nothing in the Win7 shell. |
| `ShellFixLog` | DWORD | 1 | Shell logging to `%TEMP%\7explorer-shellfix.log` (256 KB limit) plus `OutputDebugString`. **When disabled**: no log file is written (`OutputDebugString` remains active). |

## UWP / modern apps

| Name | Type | Default | Description |
|---|---|---:|---|
| `EnableImmersive` | DWORD | 0 | Upstream option: enables the immersive/UWP stack in the Win7 shell (Store apps in the Start menu/taskbar, DComp flyouts). `StoreAppsInStart`, `StoreAppsOnTaskbar`, and `UseDCompFlyouts` take effect only when this is 1. **When disabled**: UWP apps do not launch from the Win7 shell (pre-Windows 8 behavior). |
| `UwpHostRuntime` | DWORD | 1 | `ShellAppRuntime.exe` host for UWP activation: **1** = automatic (start the host only if in-process TwinUI is not running); **2** = always start the host when the shell starts; **3** = start the host **before** the Win7 desktop (reported as working in forum posts, but may take over notification-area icons; test34 does not enable it automatically); **0** = never. |
| `UwpActivationShim` | DWORD | 1 | Activates UWP apps through `IApplicationActivationManager` with a shim (foreground permissions, etc.). **When disabled**: activation falls back to the original path, which often fails without the modern shell. |
| `UwpJumpLists` | DWORD | 1 | UWP app jump lists: when the Win8+ resolver cannot find a `.lnk` (for a Store app), `shell:AppsFolder\<AUMID>` is supplied as a fallback item (test36). **When disabled**: no jump lists for UWP apps (pre-test36 behavior). Note: **Settings has no jump list by design**; the resolver is not queried for `ms-settings`. |
| `UwpEarlyHost` | DWORD | 2 | **Legacy/ignored**: once set automatically when starting the host later was insufficient (early startup at the next logon). Since test34, the value is still read but is no longer used or written automatically. To start the host early manually, use `UwpHostRuntime=3`. |
| `SettingsWin32Remap` | DWORD | 1 | Remaps `ms-settings:` URLs to equivalent classic Win32 windows/properties (Taskbar → Win7 taskbar properties, etc.). **When disabled**: `ms-settings:` uses the modern path, which fails without the Windows 11 shell. |
| `ImmersiveInitFailures` | DWORD | 0 *(state)* | Sentinel written by the code: counts startups where UWP initialization began but never succeeded. After **2** consecutive failures, UWP remains disabled until the value is reset or deleted. |

## Notification area, menus, and flyouts

| Name | Type | Default | Description |
|---|---|---:|---|
| `ClassicTrayMenus` | DWORD | 1 | Owner-drawn classic notification-area menus (ExplorerPatcher technique) for loaded notification-area modules (volume, network). **When disabled**: menus revert to those provided by the module (modern style where available). |
| `ClassicVolumeFlyout` | DWORD | 1 | Classic volume flyout: sets `EnableMTCUVC=0` for SndVolSSO so the Win7-style flyout is used (test25). **When disabled**: Windows 11's modern volume flyout. |
| `VolumeMenuActions` | DWORD | 1 | Hard-coded actions in the volume menu (Open Volume Mixer, Playback devices, etc.), because the original actions do not resolve on 24H2. **When disabled**: the volume menu has no actions. |
| `AeroFlyoutFrames` | DWORD | 1 | Aero borders/frames on legacy flyouts (credit: **aubymori**, "Aero Flyout Fix"). **When disabled**: flyouts have no Aero frame. |
| `OpaqueThumbnails` | DWORD | 0 | Opaque taskbar thumbnails with a gradient (upstream behavior) instead of translucent thumbnails. **When enabled**: thumbnails are opaque. |
| `NotifyIconsUseSettings` | DWORD | 0 | Where **Customize notification icons** opens: **0** = automatic (the system **Notification Area Icons** page, `::{05D7B0F4-…}`, if its CLSID is registered—it still exists on 24H2/25H2 but opens **blank** on 24H2—or the built-in window otherwise); **1** = Settings (`ms-settings:taskbar`); **2** = always the system page; **3** = always the **built-in window** (recreated by this project; recommended on 24H2). |
| `FixHelpAndSupportName` | DWORD | 1 | Fixes the displayed **Help and Support** name (Win7 it-IT reads the name from a missing `.mui` file). **When disabled**: the name/label in the Start menu may be incorrect. |
| `FixConnectTo` | DWORD | 1 | Registers the **Connect to** CLSID (`{38A98528-…}`, verb + TreatAs) only if the system does not already have one; opens the Video section (`shell:::{18989B1D-…}`). **When disabled**: **Connect to** may not work. |
| `FixAutoPlay` | DWORD | 1 | Repairs per-user AutoPlay policies that disable AutoPlay entirely (HKCU only): `NoDriveTypeAutoRun=0xFF` → `0x91` and `NoAutoplayfornonVolume≠0` → `0`; leaves missing values untouched and never modifies HKLM. **When disabled**: no policy repair. This option alone does not restore the classic AutoPlay dialog. |
| `AutoPlayDeviceNotifications` | DWORD | 1 | Partial **best-effort** support: listens for volume-arrival events (USB flash drives, USB drives exposed as fixed disks, and CD/DVDs) and asks the Windows shell to invoke the registered `autoplay` verb. It does not guarantee the classic AutoPlay dialog and depends on the Windows build, settings, and handlers. For the classic Windows 7 dialog, the separate Windhawk mod [Windows 7 Classic AutoPlay Dialog Restorer](https://windhawk.net/mods/win7-classic-autoplay-restorer) is recommended. **0** = do not start the listener. |
| `KeepSystemTransparency` | DWORD | 0 | By default, the private shell forces DWM transparency on (for the taskbar's Aero appearance). **When enabled**: the shell respects the system transparency setting (the taskbar is opaque if system transparency effects are off). |

## Network

| Name | Type | Default | Description |
|---|---|---:|---|
| `NetworkIconEngine` | DWORD | 1 | Network-icon engine (test32): **1** = project-owned icon driven by Network List Manager (`NetworkTrayIcon.cpp`), because the status engine in pnidui 22621 remains frozen on 24H2; **0** = previous behavior (pnidui SSO hosted by stobject). |
| `LegacyNetworkIcon` | DWORD | 1 | Downloaded legacy network icon via pnidui 22621: **1** = automatic (use the legacy pnidui only if the system does not have one); **2** = force the legacy version even if a system version exists; **0** = disabled. |
| `NetworkIconSystemGuid` | DWORD | 1 | Registers the network icon with the Win7 **system network icon** GUID (always visible, even when icons are hidden), rather than the SSO GUID (test35). **When disabled**: the icon uses the SSO GUID and may be hidden/missing. |
| `StartNetworkIcon` | DWORD | 1 | Prepares the network icon in the background when the shell starts (download + pnidui verification). **When disabled**: no network icon is prepared. |

Practical note: the component is downloaded in the background on the **first** startup; the icon appears on the **second** startup (see [troubleshooting](troubleshooting.md#network-icon-missing-on-first-startup)).

## Battery

| Name | Type | Default | Description |
|---|---|---:|---|
| `Win32BatteryFlyout` | DWORD | 1 | Battery flyout: loads Windows 8.1 stobject/batmeter (download verified from the symbol server, cached in `%LocalAppData%\7explorer\w81flyout`) with an IAT patch for `CreateWindowInBand`. **When disabled**: use the system battery flyout. |
| `W81SysTrayWrapper` | DWORD | 1 | Wraps the Windows 8.1 object in `CSysTrayWrapper` (safe lifecycle management). **When disabled**: the object is used directly. |
| `BatteryFlyoutFallback` | DWORD | 0 | Fallback when the Windows 8.1 flyout is unavailable (no battery detected, unsupported build, or download failure): show the percentage in the tooltip. **When enabled**: the percentage tooltip is always available as a fallback. |
| `W81FlyoutForce` | DWORD | 0 | 1 = ignore the supported-build table and try to load the Windows 8.1 files anyway. |
| `W81StobjectId`, `W81StobjectSha256`, `W81BatmeterId`, `W81BatmeterSha256` | SZ | — | REG_SZ overrides for an alternate Windows 8.1 build (`id` is `TimeDateStamp %08X` + `SizeOfImage %x`, as in the symbol-server URL). Both the ID and SHA-256 are required for each file. |

## Windhawk injection guard

| Name | Type | Default | Description |
|---|---|---:|---|
| `InjectionGuard` | DWORD | 1 | `LdrLoadDll` + `UnhandledExceptionFilter` hooks: everything injected into the private Explorer is classified and, if needed, blocked under SEH. **When disabled**: any DLL can be injected (risk of a shell crash). |
| `InjectionPolicy` | DWORD | 1 | **0** = do not block (log only); **1** = default: every mod is allowed to load, and only mods that **crashed** are quarantined; **≥2** = strict allow-list: unlisted Windhawk mods are blocked immediately. |
| `InjectionSwallow` | DWORD | 1 | Swallows blocked modules: `LoadLibrary` fails without crashing the caller. **When disabled**: the module is actually loaded (only if it is not quarantined). |
| `InjectionAllowlist` | MULTI_SZ | — | List of allowed Windhawk mods (path or key), one per string. Used by policy ≥2 and safe mode. |
| `InjectionBuiltinAllow` | DWORD | 1 | Built-in allow-list: `win7-network-flyout-recreation` and `win7-action-center-recreation` (written for this shell's Win7 notification area). **When disabled**: add these mods to `InjectionAllowlist` as well. |

## Upstream theme and appearance

Options inherited from explorer7, read by `explorerwrapper/OptionConfig.cpp`/`RegistryManager.cpp`:

| Name | Type | Default | Description |
|---|---|---:|---|
| `Theme` | SZ | `aero` | Theme name under `<exedir>\theme\` (relative; e.g. `aero` → `theme\aero.msstyles`, `Aero\aero` → `theme\Aero\aero.msstyles`). |
| `OrbDirectory` | SZ | *(internal)* | Directory containing orb images under `<exedir>\orbs\` (`.bmp` only; see the README). |
| `OrbFile` | SZ | *(none)* | Custom Start button image: a local `.bmp` or `.png` file (with alpha channel; absolute path or relative to `<exedir>`). Uses the three-state Open-Shell/Win7 layout: idle (0), hover (1), pressed (2); single-frame images are normalized automatically. Takes precedence over `OrbDirectory`. If missing, invalid (UNC path, unsupported extension, dimensions >1024 px, or file >4 MB), or unreadable, the preset is used, followed by the built-in image. Inspired by Open-Shell (MIT). |
| `DisableComposition` | DWORD | 0 | 1 = the shell behaves as if DWM were not active. |
| `ClassicTheme` | DWORD | 0 | 1 = Windows Classic theme. |
| `ColorizationOptions` | DWORD | 1 | Shell colorization behavior (1–4; compatibility varies). |
| `AcrylicColorization` | DWORD | 0 | Acrylic colorization (0–2 = immersive colors; 3 = regular colorization). |
| `OverrideAlpha` | DWORD | 0 | 1 = override the DWM colorization alpha for the taskbar, menus, and thumbnails. |
| `AlphaValue` | DWORD | 0x6B | Two-digit hexadecimal alpha value used with `OverrideAlpha=1`. |
| `UseTaskbarPinning` | DWORD | 1 | 0 = no taskbar pins (neither loaded nor editable from jump lists). The wrapper tries the native interfaces and, starting with Windows 11 24H2, an internal Win32 taskbar path as well. These attempts are experimental and, in the reported test, taskbar pinning **still does not work on Windows 11**. The default value 1 enables the attempt; it does not guarantee pinning or constitute a fix. |
| `StoreAppsInStart` | DWORD | 1 | 0 = hide immersive apps from **All Programs** (only when `EnableImmersive=1`). |
| `StoreAppsOnTaskbar` | DWORD | =`EnableImmersive` | 0 = do not apply/show immersive-app icons on the taskbar pins (only when `EnableImmersive=1`). |
| `UseDCompFlyouts` | DWORD | =`EnableImmersive` | Use DComp flyouts (only when `EnableImmersive=1`). |

## `config.ini` [Theme]

A `config.ini` file with a `[Theme]` section can be placed next to `explorer.exe` (see the commented [docs/config.ini.example](config.ini.example)). Selectors are handled by `explorerwrapper/ThemeManager.cpp`:

| Key | Values | Description |
|---|---|---|
| `Mode` | `Auto` *(default)* | Use a user theme from `<exedir>\theme\` if present; otherwise use the embedded theme (automatically extracted to `%LocalAppData%\7explorer\theme\aero.msstyles`). |
| | `Fallback` | Always use the embedded theme; ignore external files. |
| | `Custom` | Always try `<exedir>\theme\<Name>.msstyles`; fall back to the embedded theme if loading fails. |
| | `Windows7` | Alias for `Custom` (for supplying your own Windows 7 `aero.msstyles`). |
| | `Windows81` | Alias for `Custom` (for supplying your own Windows 8.1 `aero.msstyles`). |
| `Name` | Filename *(default `aero`, or the registry `Theme` value)* | Base name of the `.msstyles` file in `theme\` (without the extension). |

In every mode, a failed theme load falls back to the embedded theme and, ultimately, the classic look; startup is never blocked. Diagnostics: `%LocalAppData%\7explorer\theme.log`.

## Code-written state values

These values are **written** by the shell to remember state between startups; in general, do not set them manually:

| Name | Description |
|---|---|
| `StartupFailures` | Shell startups that did not reach the taskbar. At **2**, *safe mode* is triggered (mods not on the allow-list are blocked). Reset on each successful startup. Delete the value to exit safe mode. |
| `ImmersiveInitFailures` | See the UWP table above. |
| `InjectionQuarantine` | REG_MULTI_SZ: modules that crashed during startup and are blocked on subsequent restarts. Delete the value to retry quarantined modules. |

---

The switcher's options (logon startup, shortcut, recovery task) are not listed here: they use `HKCU\…\Winlogon\Shell` and the Startup folder. See [avvio-al-login.md](avvio-al-login.md).
