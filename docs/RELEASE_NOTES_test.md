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
