# switcher/ — 7explorer Shell Switcher (runtime test tool)

Small native Win32 GUI application that switches the **running** shell
process between the native Windows Explorer and the private 7explorer
Explorer7 — **no logout, no reboot, no registry changes**.

Technical notes (design is intentionally conservative):

- **Shell identification**: the shell process is the owner of
  `GetShellWindow()` (the shell desktop window). Its executable path is
  read with `QueryFullProcessImageNameW` — kernel-provided, so it is
  **not affected** by the Windhawk `ex7-fake-explorer-path` spoof (which
  only hooks `GetModuleFileNameW` inside Explorer7). Only the identified
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
- **No changes** to `C:\Windows\explorer.exe`, Winlogon, Userinit, the
  `Shell` registry value, or the registry in general.
- Builds with static CRT (`/MT`) — no Visual C++ Redistributable required.

## Paths

| shell | path |
|---|---|
| Native | `%SystemRoot%\explorer.exe` |
| 7explorer | env var `EX7_EXPLORER_PATH` (expands `%VAR%`), default `C:\ex7test\explorer.exe` |

The resolved paths are always shown in the window.

## Relationship with the Windhawk mods

Two complementary mechanisms, both kept:

| mode | how | when |
|---|---|---|
| login-time | Windhawk `ex7-userinit-shell` redirects the `Shell` query of `userinit.exe` | every logon |
| runtime test | this switcher stops/starts the shell process in place | interactive dev/test |

The Windhawk `ex7-fake-explorer-path` mod stays unchanged and remains
useful so that the runtime-launched Explorer7 believes it lives in
`%SystemRoot%`. The switcher itself does not depend on Windhawk.
