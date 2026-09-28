# Mappa di confidenza delle bozze explorer.exe.mui (2026-09-28)

Fondamentale per onestà di metodo: il file di riferimento è accessibile solo
all'utente in locale; le funzioni sotto segnate come presunte derivano da
struttura (classe, posizione, lunghezza, acceleratori, ID comando) e dalla
conoscenza dei dialog standard di Windows 7. **I testi sono tutti nostri**,
ma la FUNZIONE attribuita può essere sbagliata dove indicato. Correzioni:
modificare `localization/catalog/*.json` e far girare
`tools/verify_catalog.py` (con entrambi i constraints).

## Alti livelli (funzione quasi certa)
- Dialog 205 (scheda Menu Start) — VALIDATO dall'utente
- Dialog 6 (scheda Barra delle applicazioni) — controllo per controllo
- Dialog 10 (scheda Barre degli strumenti)
- Dialog 1036 (Personalizza menu Start) — statics/tree/conteggi coerenti
- 8224/8230/8241-8246 (azioni pulsante di alimentazione) — coerenti col
  dialog 205 (combo azione) e tra loro (Shut down / Restart / Sleep / ...)

## Medi (plausibili, verificare a schermo)
- 510-543, 580-598 (area di notifica: elenco icone, comportamenti, balloon)
- 1000-1004 (pulsanti navigazione wizard)
- 11100-11108 (barra degli strumenti/titolo/indirizzo)
- Menu 211/213 (contestuali del pulsante Start — ricostruiti dai soli cmd)
- 705/711 (testi lunghi esplicativi delle due combo impostazioni)
- 718-720 (pin/unpin/rimuovi) — acceleratori originali p/p/r compatibili

## Bassi (da rivedere DOPO il primo giro di prova)
- Menu 205/6003/12000 (pochi indizi: classic/extended + accels)
- Menu 212 (submenu "recenti" con unico elemento separatore: funzione incerta)
- Dialog 20 (272x81, icona + 3 pulsanti D/R/I) e dialog 23 (informativo con
  link + "non mostrare più"): ricostruzione funzionale generica
- 320-353 (ipotesi "personalizzazione menu Start": nomi voci albero,
  opzioni) — verificare dentro il dialog 1036 a runtime
- 1403-1423, 6010-6012, 6500-6502, 7100 (non presente), 850-858 tooltip
- 8258-8271 (14 stringhe 1-3 caratteri: ipotesi abbreviazioni calendario)

## Note operative
- Override di lunghezza IT documentati in it.json (`maxlen_override*`),
  per terminologia standard lunga — decisione del proprietario.
- I titoli finestra NON partecipano all'unicità degli acceleratori.
