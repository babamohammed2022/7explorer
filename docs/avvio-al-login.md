# Avvio automatico al logon — come funziona

> **Sintesi**: la casella *"Avvia Explorer7 automaticamente al logon"* dello
> switcher imposta il **valore `Shell` per-utente** (`HKCU`), il metodo
> standard di Windows per scegliere la shell di un utente: niente elevazione,
> niente file di sistema, completamente reversibile. In più installa due
> reti di sicurezza: un **link di fallback** nella cartella Esecuzione
> automatica e un **task di recovery** che al logon successivo controlla che
> tutto sia partito.

Il documento spiega esattamente cosa viene modificato sul sistema, come
disattivare tutto e cosa fare se qualcosa non parte.

Indice:

1. [Perché il vecchio meccanismo non bastava](#perché-il-vecchio-meccanismo-non-bastava)
2. [Cosa modifica esattamente la casella](#cosa-modifica-esattamente-la-casella)
3. [Cosa succede a ogni logon](#cosa-succede-a-ogni-logon)
4. [La scorciatoia Ctrl+Alt+Shift+S dopo il logon](#la-scorciatoia-ctrlaltshifts-dopo-il-logon)
5. [Come disattivarlo](#come-disattivarlo)
6. [Recovery: se la shell privata non parte](#recovery-se-la-shell-privata-non-parte)
7. [Variante "zero registro" (solo link)](#variante-zero-registro-solo-link)
8. [Dettagli tecnici e limiti noti](#dettagli-tecnici-e-limiti-noti)

---

## Perché il vecchio meccanismo non bastava

Fino a **test36** la casella creava solo un collegamento nella cartella
`Esecuzione automatica` dell'utente, che eseguiva
`7explorer-shell-switcher.exe --apply-ex7` **dopo** il logon. Era una gara
persa in partenza:

1. Winlogon avvia **prima** la shell di sistema (`C:\Windows\explorer.exe`,
   quella di Windows 11), che diventa proprietaria del desktop e inizializza
   il tray;
2. solo dopo, la shell già in esecuzione processa la cartella Esecuzione
   automatica: a quel punto `--apply-ex7` doveva fermare la shell nativa
   *mentre si stava ancora inizializzando* e avviare quella privata.

Risultato (il bug segnalato): al logon tornava la shell nativa di Windows 11
oppure lo switch avveniva in modo inaffidabile, in gara con l'inizializzazione
del tray. Inoltre l'istanza resident `--hotkey` muore al logoff e, se al
rientro tornava la shell nativa (che non carica `wrp64.dll`), la scorciatoia
Ctrl+Alt+Shift+S restava morta finché non si apriva la GUI.

## Cosa modifica esattamente la casella

Attivandola (test37) vengono eseguite **tre** operazioni, tutte per-utente
(HKCU / cartella profilo), tutte reversibili, senza elevazione:

| # | Cosa | Dove | A cosa serve |
|---|------|------|--------------|
| 1 | Valore `Shell` = percorso dell'explorer privato (REG_SZ) | `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon\Shell` | **Meccanismo primario**: userinit lancia l'explorer privato *come shell*, prima che parta quella nativa. Il valore precedente viene salvato (vedi sotto) e ripristinato byte per byte alla disattivazione. |
| 2 | Link `7explorer-shell.lnk` → `7explorer-shell-switcher.exe --apply-ex7 --logon` | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\` | **Fallback**: se il valore Shell non bastasse, al logon lo switcher verifica che lo switch sia davvero avvenuto (retry con backoff per ~60 s) e riavvia l'istanza `--hotkey`. |
| 3 | Task pianificato `7explorer Shell Recovery` → `7explorer-shell-switcher.exe --recover-login` | libreria Utilità di pianificazione (per-utente, trigger "al logon" con ritardo 30 s) | **Rete di sicurezza**: ~30 s dopo il logon controlla che la shell privata sia viva; se non lo è, ripristina la configurazione precedente e garantisce una shell (vedi [Recovery](#recovery-se-la-shell-privata-non-parte)). |

Note sul valore `Shell`:

- **Prima di scrivere** lo switcher verifica che nella cartella esistano sia
  `explorer.exe` (privato) sia `wrp64.dll`: il valore non punta mai a file
  mancanti. Se la verifica fallisce, la casella segnala l'errore e **non
  modifica nulla**.
- Il valore precedente viene salvato in
  `HKCU\...\Winlogon\7explorerShellBackup` (REG_BINARY: tipo + byte
  originali). Se il valore `Shell` non esisteva prima (caso normale), il
  backup registra "assente" e alla disattivazione il valore viene
  **eliminato**, riportando la chiave allo stato originale.
- Un valore marcatore `7explorerShellPath` registra la stringa esatta
  scritta da noi: serve a capire se il valore `Shell` è ancora "nostro" o se
  nel frattempo qualcun altro lo ha cambiato (in quel caso non viene toccato
  alla disattivazione).
- Se il percorso contiene spazi viene scritto tra virgolette
  (`"C:\...cartella con spazi\explorer.exe"`), come da convenzione dei
  valori Shell di Winlogon.
- Non vengono mai toccati: `HKLM`, `userinit.exe`, i file in
  `C:\Windows`, il valore `Shell` a livello macchina, o altri account.

## Cosa succede a ogni logon

1. Userinit legge `HKCU\...\Winlogon\Shell` e avvia l'explorer privato
   **come shell**: taskbar e menu Start di Windows 7 compaiono subito,
   senza che la shell nativa di Windows 11 parta per prima.
2. L'explorer privato carica `wrp64.dll` (import patchati dall'installer),
   che a sua volta:
   - gestisce il fatto che il valore `Shell` nominì un explorer esterno
     (fix *ExplorerIsShell*, test23 — altrimenti l'explorer Win7 si
     chiuderebbe come "finestra cartella");
   - forza l'avvio di desktop e taskbar (*ForceShell*, test24);
   - **riavvia automaticamente l'istanza `--hotkey`** dello switcher se la
     trova accanto a `explorer.exe`/`wrp64.dll` (opzione
     `SwitcherHotkey`, vedi sotto).
3. Il link di fallback parte con la shell: `--apply-ex7 --logon` rileva che
   la shell privata è già attiva, non fa nulla di invasivo e si assicura che
   l'istanza `--hotkey` esista, poi esce.
4. Dopo ~30 s il task `7explorer Shell Recovery` controlla che la shell
   privata sia viva. Se lo è: fine, non fa nulla (e resta installato per il
   logon successivo).

## La scorciatoia Ctrl+Alt+Shift+S dopo il logon

Con l'avvio automatico attivo, dopo il logon l'istanza resident `--hotkey`
viene avviata **da wrp64.dll** al primo avvio della shell privata
(`StartSwitcherHotkey` in `explorerwrapper/ShellFixes.cpp`, richiamata a
ogni avvio della shell). Quindi la scorciatoia di emergenza funziona da
subito, senza aprire la GUI.

Se invece al logon è tornata la shell nativa (recovery attivata, file
mancante, disattivazione), la recovery stessa e il link di fallback avviano
l'istanza `--hotkey`; in ogni caso aprira la GUI dello switcher riporta la
scorciatoia in vita. Disattivabile con `SwitcherHotkey=0` (vedi
[docs/opzioni.md](opzioni.md)).

## Come disattivarlo

- **Dalla GUI**: deseleziona la casella. Vengono ripristinati, in ordine:
  1. il valore `Shell` precedente **byte per byte** (o eliminato, se prima
     non esisteva);
  2. eliminato il task `7explorer Shell Recovery`;
  3. eliminato il link `7explorer-shell.lnk`.
- **Da riga di comando**:
  `7explorer-shell-switcher.exe --uninstall-login` (equivalente alla
  casella deselezionata).
- **A mano** (emergenza, se lo switcher non è disponibile):
  - `reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell /f`
    (se il backup `7explorerShellBackup` esiste, ripristinarne il contenuto
    a mano è opzionale: eliminare il nostro valore basta a tornare alla
    shell di sistema);
  - elimina `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\7explorer-shell.lnk`;
  - `schtasks /Delete /TN "7explorer Shell Recovery" /f`.

Se il valore `Shell` è stato modificato da qualcos'altro dopo l'attivazione,
la disattivazione **non lo sovrascrive**: lo lascia com'è e rimuove solo i
propri dati (link, task, marker/backup).

## Recovery: se la shell privata non parte

Scenario tipico: la cartella dell'explorer privato è stata spostata o
cancellata con la casella ancora attiva.

- Se il file indicato dal valore `Shell` non esiste, Windows avvia la shell
  normale del sistema (comportamento standard di userinit con una shell
  per-utente non trovabile; il task di recovery copre comunque il caso in
  cui ciò non accadesse). **È un comportamento documentato, non bloccato**:
  non serve alcun intervento manuale.
- Il task `7explorer Shell Recovery`, ~30 s dopo il logon:
  1. se non c'è **nessuna** shell in esecuzione, avvia
     `%SystemRoot%\explorer.exe`;
  2. ripristina il valore `Shell` precedente (byte per byte);
  3. elimina **se stesso** (il task non resta);
  4. avvia l'istanza `--hotkey`, così Ctrl+Alt+Shift+S funziona subito.
- Il link di fallback **non** viene rimosso dalla recovery: se esisteva,
  al logon successivo continua a provare lo switch in background (fallisce
  in silenzio finché il file manca; i tentativi sono registrati nel log).
  Per rimuoverlo: deseleziona la casella o `--uninstall-login`.

**Recovery manuale** (caso estremo, nessuna shell sullo schermo):
`Ctrl+Shift+Esc` → Gestione attività → *Esegui nuova attività* →
`explorer.exe`.

Tutte le azioni della catena logon (link, recovery, switch, registro) sono
registrate in `%TEMP%\7explorer-switcher.log` (vedi
[docs/troubleshooting.md](troubleshooting.md)).

## Variante "zero registro" (solo link)

Per chi non vuole **alcuna** modifica al registro è disponibile il solo
meccanismo di fallback:

```
7explorer-shell-switcher.exe --install-login     # crea SOLO il link
7explorer-shell-switcher.exe --uninstall-login   # rimuove tutto (anche eventuale valore Shell)
```

Il link esegue `--apply-ex7 --logon`: switch verificato, retry con backoff
per ~60 s, riavvio di `--hotkey` al successo, errori solo nel log (niente
finestre di dialogo al logon). È il comportamento delle versioni ≤ test36,
resa robusta. Limiti (motivo per cui il valore `Shell` è il meccanismo
primario): la shell nativa parte comunque per prima e viene fermata a
posteriori; su macchine lente il tray può risultare inizializzato a metà
quando avviene lo switch.

## Dettagli tecnici e limiti noti

- **Perché il valore per-utente e non HKLM**: HKLM richiede elevazione ed è
  globale per tutti gli account; il valore HKCU è per-utente, non richiede
  elevazione ed è esattamente il meccanismo pensato da Windows per le shell
  alternative per-utente.
- **Perché non un task al logon come meccanismo primario**: visivamente
  peggiore (parte prima la shell nativa, flicker, gara con il tray). Il task
  resta solo come rete di sicurezza.
- **`EX7_UI_LANG`**: la lingua UI scelta nella GUI viene passata
  all'explorer privato quando è lo switcher ad avviarlo. Quando invece è
  userinit ad avviarlo (valore Shell), la lingua è quella di sistema
  (impostabile a livello utente con la variabile d'ambiente `EX7_UI_LANG`).
- **L'explorer privato deve restare nella stessa cartella di
  `wrp64.dll`** (layout del bundle). Se la cartella viene spostata, vedi la
  sezione [Recovery](#recovery-se-la-shell-privata-non-parte).
- **Il task di recovery punta allo switcher**: se sposti l'intera cartella
  (switcher compreso), il task non trova più l'eseguibile e non può
  intervenire; in quel caso vale la recovery manuale sopra. Il valore
  `Shell` per-utente resta comunque l'unico dato modificato: eliminarlo
  riporta la shell di sistema.
- L'operazione è **per sessione utente**: fast user switching e altri
  account non sono coinvolti in alcun modo.
