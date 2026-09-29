# switcher/ — Windows 7 Explorer Restorer Shell Switcher (runtime test tool)

Small native Win32 GUI application that switches the **running** shell
process between the native Windows Explorer and the private
Windows 7 Explorer Restorer — **no logout, no reboot**.

Technical notes (design is intentionally conservative):

- **Shell identification**: the shell process is the owner of
  `GetShellWindow()` (the shell desktop window). Its executable path is
  read with `QueryFullProcessImageNameW` — kernel-provided, so it is
  **not affected** by the Windhawk `win7explorerestorer-fake-explorer-path` spoof (which
  only hooks `GetModuleFileNameW` inside the private Windows 7 Explorer Restorer process). Only the identified
  shell PID is ever stopped — never `taskkill /f /im explorer.exe`, never
  unrelated explorer instances.
- **Stop**: `WM_QUIT` to the shell window (graceful), then
  `TerminateProcess` only after a 2.5 s timeout.
- **Start**: `CreateProcessW` of the verified target path.
- **Safety**: the target must exist before anything is stopped; if the
  private explorer fails to launch, the native shell is restored
  automatically; if *both* fail, a critical message explains the manual
  recovery (Task Manager → Run new task). An *unrecognized* shell owner
  path is never stopped.
- **Never touched**: `C:\Windows\explorer.exe`, `HKLM`, `userinit.exe`,
  the machine-wide `Shell` value, other accounts. The optional logon
  auto-start (below) writes only the **per-user**
  `HKCU\...\Winlogon\Shell` value, reversibly and with its previous
  content backed up.
- Builds with static CRT (`/MT`) — no Visual C++ Redistributable required.
- Log: `%TEMP%\7explorer-switcher.log` (switches, logon auto-start,
  recovery; see `docs/troubleshooting.md`).

## Paths (no hardcode)

| shell | path |
|---|---|
| Native | `%SystemRoot%\explorer.exe` |
| Windows 7 Explorer Restorer | first hit of: ① env var `WIN7EXPLORERRESTORER_EXPLORER_PATH` (expands `%VAR%`) → ② `explorer.exe` **next to the switcher exe** (the bundle-zip layout: one folder for everything) → ③ legacy fallback `C:\Win7ExplorerRestorerTest\explorer.exe` |

A **Browse…** button lets you point anywhere else.

## Unified setup UI (test38)

The window shows one view at a time (same layout as the Windhawk prototype
`shell-switcher-ui-test`):

- **Setup** (shown when `explorer.exe` + `state\install.json` are missing
  next to the switcher): **Install** / **Reinstall** runs the bundled
  `Win7ExplorerRestorer.exe` hidden and non-blocking, with an indeterminate
  progress bar; the result comes from its exit code plus the tail of
  `log\Win7ExplorerRestorerSetup.log`. **Abort** terminates it and deletes a
  half-written private `explorer.exe` (a pre-launch snapshot tells
  "untouched" from "suspect"), so the state stays coherent. If the
  installer exe is missing, a clear error explains where to put it.
- **Main** (shown when installed): pick the shell with the radios (the live
  one is marked "– in use"), **Browse…** for another path, the logon
  checkbox plus **More information** (opens `docs/avvio-al-login.md` when
  shipped, else a built-in summary), **Reinstall** / **Uninstall** links,
  the private-shell language combo and the theme button. The footer button
  is explicit ("Use …") and warns that the desktop restarts briefly.

Flows: **Reinstall** switches to the native shell first when ours is live,
then installs and offers to switch back; **Uninstall** (confirmed) switches
to native, disables the logon auto-start and deletes only the private
`explorer.exe`, `cache\` and `state\` — never `C:\Windows\explorer.exe`,
never `HKLM`. The private-shell language combo (System default + en/it/de/es/fr/ja/pl/pt-BR/ru/zh-CN) affects **only our shell**:
it is stored per-user under `HKCU\Software\7explorer\ShellSwitcher` and
applied via the `WIN7EXPLORERRESTORER_UI_LANG` child-process environment at
switch time. All CLI flags stay headless and never start the installer.

## Login-time auto-start (test37)

The checkbox **"Start Windows 7 Explorer automatically at logon"** arms
three cooperating mechanisms (full details in
[`docs/avvio-al-login.md`](../docs/avvio-al-login.md)):

1. **Per-user `Shell` value** (primary):
   `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon\Shell` =
   the private explorer.exe. This is the standard per-user shell mechanism:
   no elevation, current user only, previous value saved and restored
   **byte-for-byte** on removal. `explorer.exe` **and** `wrp64.dll` are
   validated before anything is written.
2. **Startup-folder link** (fallback, `7explorer-shell.lnk` →
   `--apply-win7explorerestorer --logon`): verifies the switch really happened (retry with
   backoff, ~60 s) and restarts the `--hotkey` resident on success.
3. **Recovery task** (`7explorer Shell Recovery`, per-user scheduled task):
   ~30 s after each logon, if the private shell is not alive it restores
   the previous `Shell` value, makes sure some shell is running, restarts
   the `--hotkey` resident and removes itself.

Unchecking the box undoes all three. `--install-login` installs **only**
the link (the zero-registry variant, mechanism B); `--uninstall-login`
removes everything, like unchecking the box.

## Command line

```
shell-switcher.exe              # GUI
--apply-win7explorerestorer        # switch to Windows 7 Explorer Restorer (verified + retry ~60 s + hotkey restart;
                   #   exit code 0/2)
--apply-win7explorerestorer --logon  # as above, from the logon link: fully silent (log only)
--apply-native     # switch back to the native shell
--install-login    # create ONLY the Startup-folder link (zero registry changes)
--uninstall-login  # remove the whole logon auto-start (restore Shell value,
                   #   delete link + recovery task)
--hotkey           # resident Ctrl+Alt+Shift+S instance (one per session)
--recover-login    # body of the recovery task (no UI; do not run by hand)
--lang=<code>        # force the switcher UI language
                   #   (en it de es fr ja pl pt-BR ru zh-CN; default: system
                   #   language, English fallback; same codes work in
                   #   WIN7EXPLORERRESTORER_LANG; process-local only)
```

## Hotkey: Ctrl+Alt+Shift+S

A tiny resident instance (`--hotkey`, one per session, guarded by the mutex
`Local\7explorer.ShellSwitcher.Hotkey`) owns the global shortcut and opens
the switcher GUI — even when the shell has crashed or hangs. It is started:

- by the GUI itself;
- by `wrp64.dll` when the private shell starts (so it is alive right after
  every logon with the auto-start enabled — see `StartSwitcherHotkey` in
  `explorerwrapper/ShellFixes.cpp`);
- by `--apply-win7explorerestorer`/`--apply-native`/`--recover-login` after a successful
  switch/recovery.

If no instance is running (e.g. native shell after a failed logon start and
no recovery happened), the shortcut is dead: open the switcher GUI once to
bring it back.

## Relationship with the Windhawk mods

The Windhawk `win7explorerestorer-fake-explorer-path` mod is now redundant (the path spoof
is built into `wrp64.dll`) and `win7explorerestorer-userinit-shell` is superseded by the
per-user `Shell` value (test37). Both mods remain in `windhawk/` as
optional source. The switcher itself does not depend on Windhawk.
