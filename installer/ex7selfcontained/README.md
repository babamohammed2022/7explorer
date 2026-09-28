# ex7selfcontained

Bootstrap self-contained per explorer7: scarica **una sola volta** il binario
Microsoft (`explorer.exe` Win7 SP1 x64) dal symbol server, lo verifica con
SHA-256 fissato nel codice, patcha gli import verso `wrp64.dll` e inietta il
catalogo linguistico incorporato. Offline dal secondo avvio.

Vedi `docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md` per l'analisi completa e lo
stato di verifica di ogni costante.

## Build (Windows, con Visual Studio 2022 o il toolset v143)

```bat
msbuild installer\ex7selfcontained\ex7selfcontained.vcxproj /p:Configuration=Release /p:Platform=x64
```

oppure da "x64 Native Tools Command Prompt":

```bat
cd installer\ex7selfcontained
cl /std:c++17 /utf-8 /O2 /W4 /EHsc main.cpp downloader.cpp winhash.cpp importpatch.cpp localizer.cpp /link bcrypt.lib wintrust.lib crypt32.lib wininet.lib
```

`lang_catalog.h` è GENERATO: non modificarlo a mano.

```bat
python tools\embed_catalog.py
```

## Verifica delle costanti Microsoft PRIMA del primo rilascio

I valori `kTimeDateStamp`, `kSizeOfImage`, `kExpectedSha256` in `config.h`
vanno confermati sulla tua macchina (nel sandbox non sono verificabili):

```bat
curl.exe -L -o %TEMP%\explorer-ref.exe "https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe"
certutil -hashfile %TEMP%\explorer-ref.exe SHA256
python tools\analyze_mui.py %TEMP%\explorer-ref.exe --dump-headers
```

`certutil` deve stampare esattamente
`5769e5b25c7bfbc20dbfdca2f17b751f6d968e03412705de4a16c99b2626e21b`
e `--dump-headers` deve mostrare `TimeDateStamp 0x4CE7A144`,
`SizeOfImage 0x2C0000`, machine `0x8664` e gli import
`SHLWAPI.DLL`, `OLE32.DLL`, `EXPLORERFRAME.DLL`.
Se qualunque valore differisce, NON eseguire l'installer e segnalalo.

## Esecuzione

```bat
ex7selfcontained.exe --app-dir "X:\Program Files\explorer7"
```

- primo avvio: scarica + verifica + patch + iniezione lingue;
- avvii successivi (anche con `--offline`): riusa `cache\explorer-*.pris`
  riverificandone l'hash; niente rete se l'hash corrisponde.

Mai eseguire a logon in modo bloccante: deadline complessive, cancellazione
immediata a logoff/shutdown e nessuna scrittura fuori dalla cartella app.
