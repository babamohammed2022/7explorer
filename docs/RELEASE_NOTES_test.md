# Pre-release di PROVA — ex7 self-contained bootstrap

Binari compilati dal CI (GitHub Actions, `windows-latest`, MSVC) nel workflow
`selfcontained-ci`. Hash in `SHA256SUMS.txt`.

## Cosa c'è qui

| File | Cosa è |
| --- | --- |
| `wrp64.dll` | il wrapper explorer7 compilato in Release x64 |
| `ex7selfcontained.exe` | l'installer/bootstrap self-contained |
| `SHA256SUMS.txt` | SHA-256 degli eseguibili sopra |

## Cosa FUNZIONA (verificato nel CI su file reale)

- Download di `explorer.exe` (Win7 SP1 x64) dal symbol server Microsoft con
  verifica SHA-256 pinnato, dimensione esatta 2.872.320 byte, controllo
  identità PE (TimeDateStamp 0x4CE7A144, SizeOfImage 0x2C0000, AMD64);
  timeout/deadline/cancellazione a logoff-shutdown; riuso offline della copia
  verificata.
- Patch import `SHLWAPI.DLL`/`OLE32.DLL`/`EXPLORERFRAME.DLL` → `wrp64.dll`:
  **deterministica**, **idempotente**, C++ byte-identico al riferimento
  Python (confronto `fc /b` nel CI), import risultanti verificati nel log.
- Stringhe shell32 per pin/unpin menu Start in 10 lingue, testi originali,
  verificate dal checker (segnaposto/acceleratori/lunghezze) e servite dal
  fallback del wrapper.

## Cosa NON fa ancora

- **Nessuna integrazione shell** (switch di userinit/Winlogon): questa
  release testa solo download+verifica+patch+stringhe.
- Menu/dialog/acceleratori `explorer.exe.mui`: in attesa dell'elenco ID
  confermato; la neutralizzazione `MUI`→`CUI` resta dietro il cancelletto di
  copertura e richiede `--allow-partial-localization`.
- Test su macchina reale con shell avviata: da fare su Windows 10/11.

## Come provare (Windows 10 21H2 LTSC e superiori)

```bat
mkdir X:\ex7test
copy wrp64.dll X:\ex7test\
copy ex7selfcontained.exe X:\ex7test\
cd /d X:\ex7test
ex7selfcontained.exe
:: atteso: download una tantum, hash verificato, patch applicata,
:: explorer.exe patchata in X:\ex7test\, log in log\ex7setup.log
ex7selfcontained.exe --offline
:: atteso: nessuna rete, riuso della copia verificata
```

Mai puntare `--app-dir` dentro `C:\Windows`: l'installer non tocca i file di
sistema per costruzione.

## Novita' v0.0.2-test1 — localizzazione COMPLETA (strings + menus + dialogs)

- Catalogo explorer.exe.mui completo: **161 stringhe, 6 menu, 6 dialog** in
  inglese (fallback) e **italiano**, testi nostri verificati dal checker
  (segnaposto/acceleratori/lunghezze) in CI. Mappa di confidenza:
  `localization/BOZZE_CONFIDENZA.md`.
- L'installer ora **trapianta** menu/dialog/acceleratori dal file di
  riferimento `explorer.exe.mui` (Win7 RTM en-US) dentro la copia privata,
  sostituendo solo i testi con il catalogo. Identita' del .mui pinnata
  (dimensione 22016, TimeDateStamp 0x4A5BC954, SHA-256 in allow-list).
- **Per la prova serve il .mui**: metti `explorer.exe.mui` accanto
  all'installer (o in `reference\`, o `set EX7_REFERENCE_MUI=percorso`).
  Senza il file: solo stringhe, menu/dialog restano en-US e la
  neutralizzazione viene rifiutata (log chiaro).
- Sonda documentata: il symbol server Microsoft NON serve .mui (404 su
  ogni chiave provata) — il file resta offline/verified-reuse by design.
