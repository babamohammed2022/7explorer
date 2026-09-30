# Windows 7 Explorer Restorer

## About

Windows 7 Explorer Restorer is a project that aims to restore the original Windows 7 Explorer shell experience on newer versions of Windows.

The project runs a private copy of the original Windows 7 SP1 `explorer.exe` together with a compatibility wrapper, allowing the Windows 7 Explorer shell to run on modern Windows versions without replacing Windows system files.

**Note: The project is provided on a **best-effort basis** and is not affiliated with or endorsed by Microsoft nor the original explorer7. This is an unofficial project created solely to improve the user experience on modern Windows. Compatibility may vary depending on the Windows version, installed components, system configuration, and future Windows updates.**

Windows 7 Explorer Restorer has been tested on:

* Windows 10 21H2
* Windows 11 24H2

It is intended to work on both Windows 10 and Windows 11, although not every feature of the original Windows 7 shell can be guaranteed on every version.

## Personalizzazione pulsante Start (orb)

Windows 7 Explorer Restorer permette di personalizzare l'immagine del pulsante Start (orb) della taskbar tramite il registro di sistema (`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced`, con fallback su `HKLM`):

1. **`OrbFile`** (REG_SZ): percorso di un file `.bmp` o `.png` (con canale alfa) personalizzato locale (assoluto, ad esempio `C:\orbs\start.png` o `C:\orbs\start.bmp`, oppure relativo alla cartella di `explorer.exe`).
2. **`OrbDirectory`** (REG_SZ): nome di una cartella di preset in `<exedir>\orbs\<nome>\` contenente immagini (`.bmp` o `.png`) dedicate per DPI e posizione della taskbar (`6801` .. `6812`).
3. **Immagine integrata**: fallback finale incorporato in `explorer.exe`, utilizzato se `OrbFile` o `OrbDirectory` non sono configurati, non esistono o contengono file non validi.

L'ordine di precedenza applicato è **`OrbFile` > `OrbDirectory` > immagine integrata**. I file PNG vengono decodificati tramite il componente di sistema WIC (Windows Imaging Component) e convertiti in DIB section a 32 bit con canale alfa premoltiplicato (`32bppPBGRA`). L'immagine viene gestita secondo la classica struttura a 3 stati di Open-Shell/Windows 7: frame 0 per stato idle (normale), frame 1 per hover (mouse over) e frame 2 per premuto (menu Start aperto); se viene fornita un'immagine a frame singolo, questa viene normalizzata automaticamente replicando i 3 stati. In caso di errore durante la lettura, la decodifica o l'allocazione, il wrapper ricorre in modo sicuro tramite guardie SEH e RAII al livello successivo senza causare blocchi o crash all'avvio di Explorer.

*Ispirato a Open-Shell (MIT), che ha reso popolare la sostituzione del pulsante Start con immagini scelte dall'utente (inclusi PNG a 32 bit). Nessun codice di Open-Shell è incluso.*

## Emergency Shell Switcher

Since Explorer is responsible for the Windows shell, an incompatible configuration could potentially leave the desktop without a normal shell.

The project therefore includes an Emergency Shell Switcher that allows the user to switch between:

* Windows 7 Explorer
* Native Windows Explorer

This provides a recovery mechanism if Windows 7 Explorer fails to start correctly.

The shell switcher is designed to avoid requiring a permanent modification of Windows system files.

## Project Status

This project tries to make the UWP apps (Settings, Snipping tool, etc) run on the Windows 7 explorer and tries to reduce the incompatibility.

### Working

The following functionality is currently working or partially working:

* Windows 7 Explorer shell running on modern Windows
* Windows 7-style desktop and taskbar
* Compatibility wrapper for modern Windows
* Reversible shell configuration
* Emergency shell switching
* Windows 7 Explorer running without replacing the system `explorer.exe`
* Operation on both Windows 10 and Windows 11

### Known Limitations

Some Windows functionality was tightly coupled to Windows 7 system components and cannot be reproduced perfectly on modern Windows.

Known limitations may include:

* UWP/modern Windows applications
* Autoplay integration
* Some modern notification and system-tray functionality
* Modern Windows shell integrations
* Features depending on Windows 7 system DLLs or services
* Compatibility issues introduced by future Windows updates

The project should therefore be considered a restoration project rather than a complete replacement for the modern Windows shell.

## Screenshots

Screenshots will be added as the project progresses.

This section is currently a work in progress.

## Quick Start

1. Download or build the required project components.
2. Make sure the required Windows 7 Aero theme is installed and active.
3. Configure the Windows 7 Explorer shell using the provided shell switcher.
4. Log out and back in, or restart the shell if necessary.
5. If Windows 7 Explorer does not work correctly, use the Emergency Shell Switcher to return to Native Windows Explorer.

## Requirements

* Windows 10 or Windows 11
* x64 system
* Original Windows 7 SP1 Explorer components required by the project
* `wrp64.dll` compatibility wrapper
* Windows 7 Aero visual style/theme

### Windows 7 Aero Theme Requirement

A Windows 7 Aero theme is **required** for the shell to function.

The Windows 7 Explorer shell was designed around the Windows 7 Desktop Window Manager and visual style system. Without the appropriate Aero theme, graphical elements and shell functionality will not work correctly.

Other visual styles or themes can theoretically be tried, but they are untested with this project and not supported: if anything looks or behaves incorrectly, switch back to a Windows 7 Aero theme first.

The project does not replace Windows system files to provide the required theme.

## Automatic Startup at Logon

Windows 7 Explorer can be configured to start automatically when the user logs in.

The project provides a reversible shell configuration rather than replacing the native Windows `explorer.exe`.

If necessary, the user can restore Native Windows Explorer using the Emergency Shell Switcher.

## Notes

### Private Explorer Copy

The project uses a private copy of the Windows 7 Explorer executable.

The native Windows `explorer.exe` is **not replaced or modified**.

This is intended to make the installation safer and easier to reverse.

### Reversible Shell Configuration

The project changes the shell configuration required to start Windows 7 Explorer instead of Native Windows Explorer.

The configuration can be reverted to Native Windows Explorer using the provided shell switcher.

### No System File Replacement

Windows 7 Explorer Restorer does not replace Windows system files.

Modern Windows system components remain untouched.

The project instead relies on its own compatibility components and a private copy of Windows 7 Explorer.

### Offline Operation

The project is designed to operate locally and does not require an online service to function.

### Windows 10 and Windows 11 Compatibility

The project has been tested on:

* Windows 10 21H2
* Windows 11 24H2

Other Windows 10 and Windows 11 releases may work, but they have not necessarily been tested.

Compatibility should therefore be considered **best effort**.

### Windows Updates

Future Windows updates may introduce compatibility problems by changing shell components, APIs, security restrictions, or other system behavior.

## Documentation

Additional documentation is available in the `docs` directory.

Documentation includes:

* Installation
* Startup at logon
* Shell switching
* Troubleshooting
* Compatibility information

## Repository Components

The repository contains the components required to run Windows 7 Explorer on modern Windows systems.

The main components include:

* Windows 7 Explorer
* `wrp64.dll` compatibility wrapper
* Emergency Shell Switcher
* Startup and shell configuration components
* Documentation

## How It Works

Windows 7 Explorer was designed to operate against the Windows 7 shell and system environment.

Modern Windows versions have changed or removed several of the APIs and behaviors expected by the original Explorer.

Windows 7 Explorer Restorer works around these compatibility differences by running a private Windows 7 Explorer copy together with a compatibility layer.

The native Windows Explorer executable is not overwritten.

The general architecture is:

```text
Windows 10 / Windows 11
        |
        v
Windows 7 Explorer
        |
        v
wrp64.dll compatibility layer
        |
        v
Modern Windows system components
```

This approach allows the original Windows 7 Explorer shell to run while keeping the modern Windows installation itself intact.

## Building

The project can be built from the source code using the provided build instructions.

Refer to the documentation for the required development environment and build steps.

The project is primarily intended for x64 Windows systems.

## Design Goals

The project follows several main principles:

* Preserve the original Windows 7 Explorer experience
* Avoid replacing Windows system files
* Keep the installation reversible
* Minimize modifications to the operating system
* Prefer compatibility layers over permanent system modifications
* Provide a recovery mechanism when Explorer fails
* Support both Windows 10 and Windows 11 where technically possible
* Keep the project focused on Explorer and shell restoration

## License

The licensing of the original Explorer7 project and its source code must be respected.

This project is based on or inspired by existing open-source work. Refer to the original project and the included source files for the applicable licensing terms.

An archived copy of the original upstream repository is also preserved through the Wayback Machine for historical reference:

https://web.archive.org/web/20260929093957/https://github.com/world-windows-federation/explorer7

## Credits

* **World Windows Federation / Explorer7** — original project and foundation
* **Anixx** — technical inspiration and research related to downloading and handling Windows resources
* **Aubymori** — Aero/tray-related fixes and technical references
* **m417z** — Taskbar context-menu inspiration
* **ExplorerPatcher** - Windows 11 compatibility analysis with the Win32 taskbar (which is the one Windows 7's relies on)
* **Explorer7 developers** — original implementation and research

## Acknowledgements

Special thanks to the developers and researchers who have worked on restoring Windows 7 shell functionality on newer versions of Windows.

This project would not be possible without the reverse-engineering work, compatibility research, and open-source projects that document the behavior of the Windows shell.

## Project Status

Windows 7 Explorer Restorer is currently an **alpha / work-in-progress project**.

It is functional on supported configurations, but compatibility is not guaranteed on every configuration.

The project is provided on a **best-effort basis** and should be tested carefully before being used as the primary Windows shell.

The native Windows Explorer shell remains available through the Emergency Shell Switcher for recovery purposes.
