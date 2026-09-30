# windhawk/ — Windows 7 Explorer Restorer shell PoC (OPTIONAL mods)

> **Current status (test37)**: both mod features are now provided by the project itself, so these mods are **optional/supplementary**:
>
> - `win7explorerestorer-fake-explorer-path` (spoofs `GetModuleFileNameW`) has been **built into `wrp64.dll`** since test4; the mod is **redundant**.
> - `win7explorerestorer-userinit-shell` (redirects the `Shell` query from Userinit) has been **superseded** by the switcher's automatic logon startup (test37: per-user `Shell` value in HKCU + fallback + automatic recovery; see `docs/avvio-al-login.md`), which does not require Windhawk.
>
> The source files remain available for transparency/inspection and for anyone who prefers to manage logon through Windhawk.

These are two **source-only** Windhawk mods (Windhawk compiles them locally when enabled) that make the private Windows 7 Explorer Restorer copy the **active shell**, using the same techniques as Anixx's reference mods:

| File | Target | Hook | Behavior |
|---|---|---|---|
| `win7explorerestorer-userinit-shell.cpp` | `userinit.exe` | `RegQueryValueExW` | When `Shell` is queried, returns the private Explorer path (default `C:\Win7ExplorerRestorerTest\explorer.exe`); nothing is written to Winlogon/the registry. If the file is missing, it falls back to the original API (normal shell). |
| `win7explorerestorer-fake-explorer-path.cpp` | `explorer.exe` | `GetModuleFileNameW` | For `hModule==NULL`, returns `%SystemRoot%\explorer.exe` (the file on disk is not modified). |

These mods **do not** modify `C:\Windows\explorer.exe`, **do not** require manual registry editing, and **are not permanent**: disabling them restores the previous behavior at the next logon.

## Prerequisites

1. Run `Win7ExplorerRestorer.exe` successfully (it creates `C:\Win7ExplorerRestorerTest\explorer.exe` and `C:\Win7ExplorerRestorerTest\wrp64.dll`); see `installer/Win7ExplorerRestorer/README.md`. If you used `--app-dir OTHER`, set `ExplorerPath` accordingly in the mod settings.
2. Install Windhawk (standard installer from ramensoftware.com; no network access is needed after installation).

## Quick test (5 minutes)

1. Enable the **`Win7ExplorerRestorer shell launcher`** mod in Windhawk (`ExplorerPath` = `C:\Win7ExplorerRestorerTest\explorer.exe`, which is already the default).
2. Enable **`Win7ExplorerRestorer fake path`**.
   - To load local mods: in Windhawk, open **Mod settings** in the lower-right corner → enable **Developer mode** → **Home** → **Mod development** → **New mod** → paste the source (or choose *Load mod from disk* and point to this folder) → **Compile** → enable the mod.
3. **Sign out** of the session (Start → account → **Sign out**).
4. Sign back in. `userinit.exe` starts, and its `Shell` query receives the private path → **the Windows 7 taskbar should appear** in place of the Windows 11 taskbar.

### Expected result

```
Windows 11 logon
   → userinit.exe
   → Shell = C:\Win7ExplorerRestorerTest\explorer.exe          (mod response, not registry value)
   → Windows 7 Explorer Restorer starts (the other mod makes it think it is C:\Windows\explorer.exe)
   → the Windows 7 taskbar appears
   → the Windows 11 taskbar is no longer active
```

Mod logs (Windhawk → mod → **Log** tab / **Enable logging**): look for lines such as `win7explorerestorer-userinit-shell: Shell query intercepted` / `target ... exists ->` / `target MISSING ... fallback` and `win7explorerestorer-fake-explorer-path: path spoof enabled`.

## Reverting (reversible)

- **Normal case**: disable `Win7ExplorerRestorer shell launcher` in Windhawk → sign out and back in → the normal Windows shell returns.
- **If Windows 7 Explorer Restorer crashes** and leaves a black screen with a cursor: press `Ctrl+Shift+Esc` → Task Manager → *File → Run new task* → `cmd` → open Windhawk from there, disable the mod, then sign out.
- **Worst case**: boot into **Safe Mode** (Windhawk does not start in Safe Mode) and disable the mod.
- The registry `Shell` value is **never modified**: even if the mod breaks, Windows always restarts with the official shell.

## Technical notes

- Faithful registry semantics: a "size-only" query (`lpData==NULL`) returns the required `*lpcbData` and `REG_SZ`; a buffer that is too small returns `ERROR_MORE_DATA` with the required size; size is always in bytes, including the trailing NUL.
- The target's existence is checked **on every interception**. If you move the file, fallback happens immediately, without rebuilding.
- `compat/` contains only a stub used for the CI compile check; the actual `windhawk_api.h` is provided by Windhawk on the test PC.
