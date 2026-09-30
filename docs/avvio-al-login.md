# Automatic startup at logon: how it works

> **Summary**: the switcher's **"Start Win7ExplorerRestorer automatically at logon"** checkbox sets the per-user **`Shell` value** (`HKCU`), Windows' standard mechanism for choosing a user's shell. It requires no elevation, changes no system files, and is fully reversible. It also installs two safeguards: a **fallback shortcut** in the Startup folder and a **recovery task** that checks at the next logon that everything started correctly.

This document explains exactly what is changed on the system, how to disable it, and what to do if something fails to start.

Contents:

1. [Why the previous mechanism was insufficient](#why-the-previous-mechanism-was-insufficient)
2. [Exactly what the checkbox changes](#exactly-what-the-checkbox-changes)
3. [What happens at each logon](#what-happens-at-each-logon)
4. [Ctrl+Alt+Shift+S after logon](#ctrlaltshifts-after-logon)
5. [How to disable it](#how-to-disable-it)
6. [Recovery if the private shell does not start](#recovery-if-the-private-shell-does-not-start)
7. ["Zero registry" variant (shortcut only)](#zero-registry-variant-shortcut-only)
8. [Technical details and known limitations](#technical-details-and-known-limitations)

---

## Why the previous mechanism was insufficient

Up to **test36**, the checkbox created only a shortcut in the user's Startup folder that ran `shell-switcher.exe --apply-win7explorerestorer` **after** logon. This was a race that was lost from the start:

1. Winlogon first starts the **system shell** (`C:\Windows\explorer.exe`, the Windows 11 shell), which takes ownership of the desktop and initializes the notification area.
2. Only afterwards does the already-running shell process the Startup folder. At that point, `--apply-win7explorerestorer` had to stop the native shell *while it was still initializing* and start the private one.

As a result (the reported bug), the native Windows 11 shell could return at logon, or the switch could happen unreliably while racing with notification-area initialization. In addition, the resident `--hotkey` instance exits at logoff; if the native shell returned at the next logon (and therefore did not load `wrp64.dll`), the `Ctrl+Alt+Shift+S` shortcut stayed inactive until the GUI was opened.

## Exactly what the checkbox changes

When enabled (test37), it performs **three** operations. All are per-user (`HKCU` / the user profile), reversible, and require no elevation:

| # | What | Location | Purpose |
|---|------|----------|---------|
| 1 | `Shell` value = path to the private Explorer (REG_SZ) | `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon\Shell` | **Primary mechanism**: Userinit launches the private Explorer *as the shell*, before the native shell starts. The previous value is saved (see below) and restored byte-for-byte when disabled. |
| 2 | `7explorer-shell.lnk` shortcut → `shell-switcher.exe --apply-win7explorerestorer --logon` | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\` | **Fallback**: at logon, the switcher verifies that the shell switch actually succeeded (retry with backoff for about 60 seconds) and restarts the `--hotkey` instance. |
| 3 | Scheduled task `7explorer Shell Recovery` → `shell-switcher.exe --recover-login` | Task Scheduler library (per-user, "at logon" trigger delayed by 30 seconds) | **Safeguard**: about 30 seconds after logon, checks that the private shell is alive; if not, restores the previous configuration and ensures that a shell is running (see [Recovery](#recovery-if-the-private-shell-does-not-start)). |

Notes about the `Shell` value:

- **Before writing it**, the switcher verifies that both `explorer.exe` (the private copy) and `wrp64.dll` exist in the folder. The value never points to missing files. If the check fails, the checkbox reports an error and **nothing is changed**.
- The previous value is saved in `HKCU\...\Winlogon\7explorerShellBackup` (REG_BINARY: type + original bytes). If the `Shell` value did not exist before (the normal case), the backup records it as "absent" and disabling the feature **deletes** the value, restoring the key to its original state.
- A marker value, `7explorerShellPath`, records the exact string written by this project. It is used to determine whether `Shell` is still "ours" or has since been changed by something else (if it has, disabling the feature leaves it untouched).
- If the path contains spaces, it is written in quotes (for example, `"C:\...folder with spaces\explorer.exe"`), as required by the Winlogon `Shell` value convention.
- The following are never modified: `HKLM`, `userinit.exe`, files under `C:\Windows`, the machine-wide `Shell` value, or other user accounts.

## What happens at each logon

1. Userinit reads `HKCU\...\Winlogon\Shell` and starts the private Explorer **as the shell**. The Windows 7 taskbar and Start menu appear immediately, without first starting the native Windows 11 shell.
2. The private Explorer loads `wrp64.dll` (its imports were patched by the installer), which in turn:
   - handles the fact that the `Shell` value names an external Explorer (the *ExplorerIsShell* fix, test23; otherwise Win7 Explorer would exit as a "folder window");
   - forces the desktop and taskbar to start (*ForceShell*, test24);
   - **automatically restarts the switcher's `--hotkey` instance** if it finds the switcher next to `explorer.exe`/`wrp64.dll` (the `SwitcherHotkey` option, described below).
3. The fallback shortcut starts with the shell. `--apply-win7explorerestorer --logon` detects that the private shell is already active, makes no disruptive changes, ensures that the `--hotkey` instance is running, and exits.
4. After about 30 seconds, the `7explorer Shell Recovery` task checks that the private shell is alive. If it is, the task does nothing (and remains installed for the next logon).

## Ctrl+Alt+Shift+S after logon

When automatic startup is enabled, `wrp64.dll` starts the resident `--hotkey` instance at the private shell's first startup (`StartSwitcherHotkey` in `explorerwrapper/ShellFixes.cpp`, called on each shell startup). The emergency shortcut therefore works immediately, without opening the GUI.

If the native shell returned at logon (recovery was triggered, a file is missing, or automatic startup was disabled), recovery and the fallback shortcut start the `--hotkey` instance. In any case, opening the switcher GUI brings the shortcut back. Disable it with `SwitcherHotkey=0` (see [docs/opzioni.md](opzioni.md)).

## How to disable it

- **In the GUI**: clear the checkbox. The following items are restored or removed, in order:
  1. the previous `Shell` value, byte-for-byte (or the value is deleted if it did not exist before);
  2. the `7explorer Shell Recovery` task;
  3. the `7explorer-shell.lnk` shortcut.
- **From the command line**: run `shell-switcher.exe --uninstall-login` (equivalent to clearing the checkbox).
- **Manually** (emergency, if the switcher is unavailable):
  - `reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell /f` (if the `7explorerShellBackup` value exists, manually restoring its contents is optional; deleting our value is enough to return to the system shell);
  - delete `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\7explorer-shell.lnk`;
  - run `schtasks /Delete /TN "7explorer Shell Recovery" /f`.

If something else changed the `Shell` value after this feature was enabled, disabling it **does not overwrite that change**: it leaves the value as-is and removes only this project's data (shortcut, task, marker, and backup).

## Recovery if the private shell does not start

Typical scenario: the private Explorer folder was moved or deleted while the checkbox was still enabled.

- If the file named by the `Shell` value does not exist, Windows starts its normal system shell (the standard Userinit behavior when a per-user shell cannot be found; the recovery task also covers cases where this does not happen). **This is documented behavior, not a lockout**; no manual intervention should be needed.
- About 30 seconds after logon, the `7explorer Shell Recovery` task:
  1. starts `%SystemRoot%\explorer.exe` if **no** shell is running;
  2. restores the previous `Shell` value byte-for-byte;
  3. deletes **itself** (the task does not remain installed);
  4. starts the `--hotkey` instance so that `Ctrl+Alt+Shift+S` works immediately.
- Recovery does **not** remove the fallback shortcut. If present, it continues trying the switch in the background at the next logon (silently failing while the file is missing; attempts are logged). Remove it by clearing the checkbox or running `--uninstall-login`.

**Manual recovery** (extreme case: no shell is visible): press `Ctrl+Shift+Esc` → Task Manager → *Run new task* → `explorer.exe`.

All logon-chain actions (shortcut, recovery, shell switch, registry) are recorded in `%TEMP%\7explorer-switcher.log` (see [docs/troubleshooting.md](troubleshooting.md)).

## "Zero registry" variant (shortcut only)

For users who do not want **any** registry changes, the fallback-only mechanism is available:

```
shell-switcher.exe --install-login     # creates ONLY the shortcut
shell-switcher.exe --uninstall-login   # removes everything (including any Shell value)
```

The shortcut runs `--apply-win7explorerestorer --logon`: it verifies the switch, retries with backoff for about 60 seconds, restarts `--hotkey` on success, and writes errors only to the log (no dialog boxes at logon). This is the robust version of the behavior used through test36. Limitations (and the reason the `Shell` value is the primary mechanism): the native shell still starts first and is stopped afterwards; on slow machines, the notification area may be only partially initialized when the switch occurs.

## Technical details and known limitations

- **Why use a per-user value instead of `HKLM`**: `HKLM` requires elevation and applies to all accounts. The `HKCU` value is per-user, requires no elevation, and is exactly the mechanism Windows provides for per-user alternative shells.
- **Why not use a logon task as the primary mechanism**: it looks worse (the native shell starts first, causing flicker and a race with the notification area). The task remains only as a safeguard.
- **`WIN7EXPLORERRESTORER_UI_LANG`**: the UI language selected in the GUI is passed to the private Explorer when the switcher starts it. When Userinit starts it (through the `Shell` value), the system UI language is used. This can be set per-user with the `WIN7EXPLORERRESTORER_UI_LANG` environment variable.
- **The private Explorer must remain in the same folder as `wrp64.dll`** (the bundle layout). If the folder is moved, see [Recovery](#recovery-if-the-private-shell-does-not-start).
- **The recovery task points to the switcher**: if you move the whole folder (including the switcher), the task can no longer find the executable and cannot intervene. In that case, use the manual recovery steps above. The per-user `Shell` value is still the only modified shell setting; deleting it restores the system shell.
- The operation is **per user session**: fast user switching and other accounts are not affected.
