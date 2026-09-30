# Installation

Complete installation guide, from downloading the bundle through the first startup (and later offline startups). For known issues, see [troubleshooting.md](troubleshooting.md).

Contents: [requirements](#requirements) · [procedure](#procedure) · [files downloaded and verified](#files-downloaded-and-verified) · [where files are stored](#where-files-are-stored) · [offline startup](#offline-startup) · [uninstall](#uninstall) · [HiDPI and limitations](#hidpi-and-limitations)

---

## Requirements

- **Windows 11 24H2 / 25H2, x64** (current target). Windows 8.1/10 x64 work for the most part, but are not the focus.
- An Internet connection **only for the first startup** (see the [table](#files-downloaded-and-verified)); subsequent startups work offline.
- **No elevation required**: installation is per-user and does not modify system files (never `C:\Windows\explorer.exe`, never `HKLM`).

## Procedure

1. Download **`Win7ExplorerRestorer-test-bundle.zip`** from the [reference release](https://github.com/babamohammed2022/Windows7ExplorerRestorer/releases). It contains `wrp64.dll`, `Win7ExplorerRestorer.exe`, `shell-switcher.exe`, and the README files (including source for the optional Windhawk mods).
2. Extract everything into **one folder**, for example `C:\Win7ExplorerRestorerTest`.
3. Run **`Win7ExplorerRestorer.exe`**. It:
   - downloads the Windows 7 SP1 x64 `explorer.exe` from Microsoft's symbol server and verifies its **SHA-256 hash, size, TimeDateStamp, and Authenticode signature** before using it;
   - creates a private copy of `explorer.exe` and patches its imports (`SHLWAPI.DLL`, `OLE32.DLL`, `EXPLORERFRAME.DLL` → `wrp64.dll`);
   - generates and injects the UI resources produced by this project (en-US + it-IT); **no `.mui` files** are requested, downloaded, or created.

   The operation is deterministic and repeatable: running it again produces the same file (verified by CI on every build).

   You can also skip this step: if the installation is missing, launching **`shell-switcher.exe`** opens a window offering **Install** and runs `Win7ExplorerRestorer.exe` for you (hidden, with a progress bar and details in the log).
4. Launch **`shell-switcher.exe`** → select **Windows 7 Explorer** → **Use Win7ExplorerRestorer**. The Windows 7 shell replaces the Windows 11 shell immediately.
5. To return to the native shell: press `Ctrl+Alt+Shift+S` → select **Native Windows Explorer** → **Switch**.
6. (Optional) Enable **"Start Win7ExplorerRestorer automatically at logon"** in the switcher; see [avvio-al-login.md](avvio-al-login.md).

## Files downloaded and verified

Full transparency: **the URL and SHA-256 hash of every downloaded file are pinned in the source code** (they cannot be changed at runtime); files with unexpected hashes are rejected. The project never redistributes Microsoft binaries: files are downloaded from their original servers (Microsoft's symbol server, or the ExplorerPatcher repository for the `pnidui` `.mui` files only).

### During installation (`Win7ExplorerRestorer.exe`, first startup)

| File | URL | SHA-256 (allow-list) |
|---|---|---|
| `explorer.exe` Win7 SP1 x64 (6.1.7601.17514, 2,872,320 bytes, TimeDateStamp `0x4CE7A144`, SizeOfImage `0x2C0000`) | `https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe` | `5769e5b25c7bfbc20dbfdca2f17b751f6d968e03412705de4a16c99b2626e21b` or `6a671b92a69755de6fd063fcbe4ba926d83b49f78c42dbaeed8cdb6bbc57576a` (two legitimate re-signed copies observed: a Windows 10 21H2 LTSC user system and the CI runner; identical structure, see `installer/Win7ExplorerRestorer/config.h`) |

Identity is verified at **four levels**: allow-listed hash, byte size, `TimeDateStamp`/`SizeOfImage`, and Authenticode signature. CI downloads and re-verifies the same file on every build.

### On the shell's first startup (`wrp64.dll`, in the background)

| File | URL | SHA-256 |
|---|---|---|
| `pnidui.dll` 10.0.22621.3810 x64 (network icon) | `https://msdl.microsoft.com/download/symbols/pnidui.dll/F717CABC20B000/pnidui.dll` | `52e9c88cb50ef98e683839ab62880180f0dd3db3e1c445ea461ae63d97a349d3` |
| `pnidui.dll.mui` **en-US** | `https://raw.githubusercontent.com/valinet/ExplorerPatcher/0a88a6e0ef6b1752fea36e581cffff1097e862b0/ep_setup/resources/files/pnidui/en-US/pnidui.dll.mui` | `ae0d2655cb9806b5c16b92f82d698f5c98feaef1f78ebdd6014dccc8304184ed` |
| `pnidui.dll.mui` **it-IT** | `https://raw.githubusercontent.com/valinet/ExplorerPatcher/0a88a6e0ef6b1752fea36e581cffff1097e862b0/ep_setup/resources/files/pnidui/it-IT/pnidui.dll.mui` | `e7ce6e6e43483815b79946b05e6f744e9277a123ef387485826d558533609f20` |
| `batmeter.dll` 6.3.9600.17415 x64 (battery flyout) | `https://msdl.microsoft.com/download/symbols/batmeter.dll/545054931f3000/batmeter.dll` | `f32f18d44f9a6511c73ca1a9a4a6edad38aff23a15fd4c75d9aaaaf31526a506` |
| `stobject.dll` 6.3.9600.17415 x64 (battery flyout) | `https://msdl.microsoft.com/download/symbols/stobject.dll/54503A4356000/stobject.dll` | `30737741f7131ff80706c7d12b2fe8ab8a6203aeaf9984d9a7c908c0f2565149` |

Notes:

- The `pnidui` `.mui` files exist for about 30 languages in the source (`explorerwrapper/NetworkIcon.cpp`, `kMui` table); en-US and it-IT are listed here because those are the languages localized by this project.
- The Windows 8.1 battery-flyout files are downloaded **only if** the system has a battery and its build is in the supported-build table. After a failure, the download is retried at most once every 24 hours.
- Downloads run on a background thread: the **network icon is not available on the first startup**; it appears after the shell's second startup (known issue; see [troubleshooting](troubleshooting.md#network-icon-missing-on-first-startup)).

## Where files are stored

| Path | Contents |
|---|---|
| `<bundle folder>\explorer.exe` | Private, patched, localized copy (the "working copy") |
| `<bundle folder>\cache\explorer-<ts>-<soi>.pris` | Verified **pristine** copy of the Win7 Explorer (reused offline) |
| `<bundle folder>\state\install.json` | Installation record (hashes and timestamps) |
| `<bundle folder>\log\Win7ExplorerRestorerSetup.log` | Human-readable installer log |
| `%LocalAppData%\7explorer\pnidui-F717CABC20B000\` | `pnidui` and `.mui` files for the network icon |
| `%LocalAppData%\7explorer\w81flyout\` | Windows 8.1 `batmeter`/`stobject` files for the battery flyout |
| `%LocalAppData%\7explorer\theme\aero.msstyles` | Automatically extracted embedded theme (fallback) |
| `%TEMP%\7explorer-shellfix.log` | Shell log (`wrp64.dll`) |
| `%TEMP%\7explorer-switcher.log` | Switcher log (shell switching, logon, recovery) |
| `%LocalAppData%\7explorer\theme.log` | Theme log |

## Offline startup

Starting with the **second** startup, no network connection is needed:

- `Win7ExplorerRestorer.exe` reuses the pristine copy in `cache\` (verified against its original source), or accepts the `--offline` flag to avoid any network requests.
- The shell reuses components in `%LocalAppData%\7explorer\`. If a previous download failed, the related feature simply remains disabled (network icon missing, or the system battery flyout in use) until a download succeeds.

## Uninstall

1. Disable automatic logon startup (or run `shell-switcher.exe --uninstall-login`). This restores the previous `Shell` value and removes the link and recovery task ([details](avvio-al-login.md)).
2. Switch back to the native shell (using the switcher or `Ctrl+Alt+Shift+S`).
3. Delete the bundle folder and, if desired, `%LocalAppData%\7explorer\`. That's all: no system files or `HKLM` values have ever been modified.

## HiDPI and limitations

- **HiDPI**: the Win7 Explorer is not DPI-aware like the Windows 11 Explorer. At 125%/150%, the taskbar and Start menu use classic bitmap scaling. The orb system supports dedicated images for 125%/150% (see the upstream README's **Custom orbs** section).
- The taskbar appears only on the **primary monitor** (a Win7 Explorer limitation).
- Changes to taskbar size/position are saved to the Win7 registry after a few minutes. Restarting Explorer immediately can undo them (known Win7 limitation).
