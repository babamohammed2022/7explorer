# windhawk/ — PoC shell Windows 7 Explorer Restorer (mod OPZIONALI)

> **Stato attuale (test37)**: entrambe le funzioni dei mod sono ora
> coperte dal progetto stesso e i mod sono **opzionali/supplementari**:
>
> - `win7explorerestorer-fake-explorer-path` (spoof di `GetModuleFileNameW`) è **integrato
>   in `wrp64.dll`** fin da test4 — il mod è **ridondante**;
> - `win7explorerestorer-userinit-shell` (redirect della query `Shell` di userinit) è
>   **soppiantato** dall'avvio al logon dello switcher (test37: valore
>   `Shell` per-utente in HKCU + fallback + recovery automatico, vedi
>   `docs/avvio-al-login.md`), che non richiede Windhawk.
>
> Questi sorgenti restano per trasparenza/ispezione e per chi preferisce
> gestire il logon tramite Windhawk.


Due mod Windhawk **sorgente** (Windhawk compila localmente quando li abiliti)
che rendono l'explorer privato di Windows 7 Explorer Restorer la **shell attiva**, usando le
stesse tecniche dei mod di riferimento di Anixx:

| File | Target | Hook | Cosa fa |
|---|---|---|---|
| `win7explorerestorer-userinit-shell.cpp` | `userinit.exe` | `RegQueryValueExW` | alla query `Shell` risponde con il path dell'explorer privato (default `C:\Win7ExplorerRestorerTest\explorer.exe`) — niente viene scritto in Winlogon/registry; se il file manca → fallback all'API originale (shell normale) |
| `win7explorerestorer-fake-explorer-path.cpp` | `explorer.exe` | `GetModuleFileNameW` | per `hModule==NULL` risponde `%SystemRoot%\explorer.exe` (il file su disco non viene toccato) |

**Non** modificano `C:\Windows\explorer.exe`, **non** richiedono editing
manuale del registry, **non** sono permanenti: disabilitarli riporta tutto
com'era al prossimo logon.

## Prerequisiti

1. `Win7ExplorerRestorer.exe` eseguito con successo (produce
   `C:\Win7ExplorerRestorerTest\explorer.exe` + `C:\Win7ExplorerRestorerTest\wrp64.dll`) — vedi
   `installer/Win7ExplorerRestorer/README.md`. Se hai usato `--app-dir ALTRO`,
   imposta `ExplorerPath` di conseguenza nelle impostazioni del mod.
2. Windhawk installato (installer standard da ramensoftware.com; dopo
   l'installazione non serve la rete).

## Test rapido (5 minuti)

1. Abilita in Windhawk il mod **`Win7ExplorerRestorer shell launcher`**
   (impostazione `ExplorerPath` = `C:\Win7ExplorerRestorerTest\explorer.exe`, già il default);
2. abilita **`Win7ExplorerRestorer fake path`**;
   - per caricare i mod locali: Windhawk → angolo in basso a destra **"Mod
     settings"** → attiva **"Developer mode"** → scheda **"Home" → "Mod
     development" → "New mod"** → incolla il sorgente (o usa *Load mod from
     disk* puntando a questa cartella) → **Compile** → abilita;
3. **Disconnetti** la sessione (Start → account → Disconnetti);
4. riconnetti: `userinit.exe` parte, la sua query `Shell` viene risposta con
   il path privato → **deve apparire la taskbar di Windows 7** al posto di
   quella di Windows 11.

### Risultato atteso

```
login Windows 11
   → userinit.exe
   → Shell = C:\Win7ExplorerRestorerTest\explorer.exe          (risposta del mod, non dal registry)
   → Windows 7 Explorer Restorer parte (pensa di essere C:\Windows\explorer.exe per l'altro mod)
   → compare la taskbar Windows 7
   → la taskbar Windows 11 non è più attiva
```

Log dei mod (Windhawk → mod → scheda log / enable logging): cerca righe
`win7explorerestorer-userinit-shell: Shell query intercepted` / `target ... exists ->` /
`target MISSING ... fallback` e `win7explorerestorer-fake-explorer-path: path spoof enabled`.

## Ripristino (reversibilità)

- **Caso normale**: disabilita `Win7ExplorerRestorer shell launcher` in Windhawk →
  disconnetti/riaccendi → torna la shell Windows normale.
- **Se Windows 7 Explorer Restorer crasha** e resta schermo nero col cursore:
  `Ctrl+Shift+Esc` → Task Manager → *File → Esegui nuova attività* → `cmd`
  → apri Windhawk da lì, disabilita il mod → disconnetti.
- **Worst case**: avvia in **Modalità provvisoria** (Windhawk non parte in
  safe mode) e disabilita il mod.
- Il valore `Shell` nel registry **non viene mai modificato**: anche a mod
  rotto, Windows riparte sempre con la shell ufficiale.

## Note tecniche

- Semantica registry fedele: query "size-only" (lpData==NULL) →
  `*lpcbData` richiesto + `REG_SZ`; buffer troppo piccolo →
  `ERROR_MORE_DATA` con dimensione richiesta; dimensione sempre in byte
  incl. NUL finale.
- Controllo esistenza del target **ad ogni intercettazione** (se sposti il
  file, il fallback scatta subito, senza rebuild).
- `compat/` contiene solo uno stub per il compile-check in CI; il vero
  `windhawk_api.h` è quello di Windhawk sul PC di test.
