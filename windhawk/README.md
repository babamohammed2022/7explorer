# windhawk/ — PoC shell Explorer7 su Windows 10/11

Due mod Windhawk **sorgente** (Windhawk compila localmente quando li abiliti)
che rendono l'explorer privato di 7explorer la **shell attiva**, usando le
stesse tecniche dei mod di riferimento di Anixx:

| File | Target | Hook | Cosa fa |
|---|---|---|---|
| `ex7-userinit-shell.cpp` | `userinit.exe` | `RegQueryValueExW` | alla query `Shell` risponde con il path dell'explorer privato (default `C:\ex7test\explorer.exe`) — niente viene scritto in Winlogon/registry; se il file manca → fallback all'API originale (shell normale) |
| `ex7-fake-explorer-path.cpp` | `explorer.exe` | `GetModuleFileNameW` | per `hModule==NULL` risponde `%SystemRoot%\explorer.exe` (il file su disco non viene toccato) |

**Non** modificano `C:\Windows\explorer.exe`, **non** richiedono editing
manuale del registry, **non** sono permanenti: disabilitarli riporta tutto
com'era al prossimo logon.

## Prerequisiti

1. `ex7selfcontained.exe` eseguito con successo (produce
   `C:\ex7test\explorer.exe` + `C:\ex7test\wrp64.dll`) — vedi
   `installer/ex7selfcontained/README.md`. Se hai usato `--app-dir ALTRO`,
   imposta `ExplorerPath` di conseguenza nelle impostazioni del mod.
2. Windhawk installato (installer standard da ramensoftware.com; dopo
   l'installazione non serve la rete).

## Test rapido (5 minuti)

1. Abilita in Windhawk il mod **`Explorer7 shell launcher`**
   (impostazione `ExplorerPath` = `C:\ex7test\explorer.exe`, già il default);
2. abilita **`Explorer7 fake path`**;
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
   → Shell = C:\ex7test\explorer.exe          (risposta del mod, non dal registry)
   → Explorer7 parte (pensa di essere C:\Windows\explorer.exe per l'altro mod)
   → compare la taskbar Windows 7
   → la taskbar Windows 11 non è più attiva
```

Log dei mod (Windhawk → mod → scheda log / enable logging): cerca righe
`ex7-userinit-shell: Shell query intercepted` / `target ... exists ->` /
`target MISSING ... fallback` e `ex7-fake-explorer-path: path spoof enabled`.

## Ripristino (reversibilità)

- **Caso normale**: disabilita `Explorer7 shell launcher` in Windhawk →
  disconnetti/riaccendi → torna la shell Windows normale.
- **Se Explorer7 crasha** e resta schermo nero col cursore:
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
