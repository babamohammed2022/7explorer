<!--
POLICY (not rendered in the release page): every test release MUST stay
generic and in English, following this fixed schema. Never mention what
was improved, fixed, or left unfinished. The only downloadable asset is
Win7ExplorerRestorer-test-bundle.zip (see the prerelease job in
.github/workflows/selfcontained-ci.yml).
-->

# Windows 7 Explorer Restorer

This is a test build of **Windows 7 Explorer Restorer**, a project that runs the Windows 7 Explorer shell on modern Windows, restoring the classic taskbar, Start menu, notification area, and Explorer experience.

It uses a private copy of Windows 7 Explorer together with a compatibility layer and **does not replace or modify Windows system files**.

## Download

**`Win7ExplorerRestorer-test-bundle.zip`** — Recommended bundle containing the compatibility wrapper, installer, shell switcher, documentation, and optional Windhawk sources.

## Status

This is a **test build provided on a best-effort basis**. Some features and compatibility scenarios may still be incomplete or unstable.

Windows 7 Aero theming is required/recommended for the intended appearance.

## Runtime Downloads & Transparency

Microsoft binaries are **not included in the release assets**.

Required files such as `explorer.exe`, `pnidui.dll`, `batmeter.dll`, and `stobject.dll` are downloaded at runtime from fixed URLs. Expected SHA-256 hashes are embedded in the source code, and files with unexpected hashes are rejected.

The project documents the source URLs and hashes used for these downloads. Network-related resources are downloaded only when required, with subsequent launches using the locally cached files.

## Credits

* **World Windows Federation / Explorer7** — original project and implementation
* **Explorer7 developers** — original research and technical work
* **aubymori** — Aero flyout and tray-related fixes
* **valinet / ExplorerPatcher** — tray and `pnidui.dll` resources
* **m417z** — taskbar context-menu inspiration
* **Anixx** — asset downloading mechanism

## License

GPLv3. (Same license as the original project (Proof of the 29 september of 2026: https://web.archive.org/web/20260929093957/https://github.com/world-windows-federation/explorer7))

This project is provided **as-is and on a best-effort basis**.
