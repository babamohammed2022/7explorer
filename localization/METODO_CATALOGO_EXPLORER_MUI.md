# Metodo per il catalogo linguistico di explorer.exe.mui

*(Istruzioni concordate con il proprietario del progetto il 2026-09-28:
procedura collaudata su dialog 205.)*

## Regola d'oro (copyright)

NON copiare/tradurre parola per parola il testo Microsoft nel `.mui`.
Vietato mettere il testo originale Microsoft in `localization/catalog/*.json`
(il repo è pubblico GPLv3 = redistribuzione).

**Metodo consentito**: leggere il significato/funzione della stringa
originale (in locale, mai incollata in chat o commit), poi scrivere una
frase PROPRIA — stessa funzione UI, parole diverse, niente calco della
struttura sintattica originale.

## Procedura passo-passo

1. Partire da `localization/constraints/explorer.exe.constraints.json`
   (solo struttura, zero testo: 161 stringhe, 6 menu, 6 dialog,
   1 tabella acceleratori).
2. Per ogni ID: leggere `len` (lunghezza orientativa), `accel` (lettera
   acceleratore da preservare come CONCETTO — un solo `&` per stringa,
   univoco tra i controlli/voci dello stesso dialog o menu), `placeholders`
   (es. `%s`, `%1!s!` — stesso numero/ordine/tipo, MAI cambiare).
3. Scrivere una frase originale nella lingua target che comunica la stessa
   funzione. Lunghezza libera ma entro il budget:
   `max(2 * len_originale, len_originale + 8)` caratteri (esclusi i `&`).
4. Validare SEMPRE prima di consegnare: niente doppi `&`, segnaposto
   identici per numero/ordine/tipo, acceleratori non duplicati nello stesso
   contesto (stesso dialog o stesso livello di menu), lunghezza entro
   budget.
5. Procedere A LOTTI PICCOLI (un dialog o un menu alla volta) e mostrare il
   risultato con la tabella di validazione prima di passare al lotto
   successivo — il proprietario corregge in corsa.

## Script di validazione

Gli helper stanno in `tools/analyze_mui.py` (nomi reali dell'API del repo):
`extract_accel` (solleva `ValueError` se >1 `&`), `extract_placeholders`
(solleva `ValueError` se `%` orfano), `strip_accels` (nota: plurale).
Il checker definitivo da far passare in CI resta `tools/verify_catalog.py`.

## Esempio già fatto e validato: dialog 205 (scheda "Menu Start")

| Controllo | Testo (italiano, originale) |
|---|---|
| Titolo | Menu &Start |
| 1134 (static) | Per decidere come appaiono e si comportano collegamenti, icone e menu nel menu Start, premi Personalizza. |
| 1131 (bottone) | Personali&zza... |
| 65535 (static) | Azione del p&ulsante di alimentazione: |
| 1136 (checkbox) | Mostra i &programmi aperti di recente nel menu Start |
| 1135 (checkbox) | Mostra gli elementi aperti di recente nel &menu Start e nella barra delle applicazioni |
| 300 (bottone) | Privacy |
| 1116 (link) | Come faccio a cambiare l'aspetto del menu Start? |

Risultato validazione: 0 errori, acceleratori usati nel dialog `Z U P M`
(nessun duplicato), tutte le lunghezze entro budget.

## Cosa manca ancora

- Dialog: 6, 10, 20, 23, 1036 (205 fatto)
- Menu: 205, 211, 212, 213, 6003, 12000 (nessuno fatto)
- Stringhe libere: 161 ID in `constraints → "strings"`, molte già collegate
  a un controllo/voce; le restanti da fare a blocchi da ~15-20 alla volta

## Estensione ad altre lingue

Ripetere lo stesso procedimento (leggi significato → scrivi frase
originale → valida) per ogni lingua target, salvando in
`localization/catalog/<lang>.json` con lo schema di `localization/schema.md`.
L'inglese (`en.json`) resta il fallback obbligatorio e deve coprire OGNI ID
prima delle altre lingue.
Priorità revisione madrelingua: giapponese e cinese (acceleratori su
ideogrammi), poi russo, poi le lingue latine.
