# Root cause — v0.0.2-test1: «UpdateResource failed for a string block»

*(Analisi completata 2026-09-28. L'utente ha chiesto, PRIMA di implementare
v0.0.3: (a) perché il fallimento su `UpdateResource` e (b) perché
`--allow-partial-localization` non lasciava comunque proseguire.)*

## (a) Perché `UpdateResource failed for a string block`

**Risposta provata in CI: il file è un binario language-neutral marcato MU
(risorsa RCDATA "MUI") e `UpdateResourceW` RIFIUTA di inserire risorse in un
tale binario con `ERROR_NOT_SUPPORTED (50)`.** Non era un problema di
payload, di lingua, di allineamento o di permissions: è un rifiuto
strutturale di Windows verso i PE MU-accoppiati.

Evidenza 1 — riproduzione della corsa utente in CI
(`ci-logs/diagloc-36412428950`, run `windows-latest` con il codice
strumentato): stesso fallimento dell'utente, con l'errore ora visibile:

```
FAILED: localization injection: UpdateResource failed for string block 337
(lcid 0x0407, 376 bytes), GetLastError=50
```

Evidenza 2 — probe dedicato `ci/updres/Program.cs`
(risultati in `ci-logs/updres-*`, es. run 36415198307), sul file Microsoft
reale:

| Variante | Esito |
|---|---|
| V1 scrittura strings en-US (0x0409) nel file com'è | `ok=False gle=50` ❌ |
| V2 scrittura strings de-DE (0x0407) | `ok=False gle=50` ❌ (=> non dipende dalla lingua) |
| V3a cancellazione del tipo "MUI" via `UpdateResource(...,NULL,0)` | `ok=False gle=87` ❌ (ERROR_INVALID_PARAMETER) |
| V4 del-MUI + write nella stessa transazione | delMUI `gle=87`, write `gle=1359` ❌ |
| **V5 `BeginUpdateResource(bDeleteExistingResources=TRUE)` + riscrittura completa** | **ok ✅** |

Conclusione: l'unica via che Windows accetta su questo binario è la
**riscrittura atomica dell'intera tabella risorse** — che è esattamente
quello che fa v0.0.3 (`LocalizeWithGeneratedResources`: enumera tutto,
ricopia tutto, omette "MUI" (parcheggiato come "CUI"), scrive i blob
generati, committa una sola transazione).

Conseguenza storica: l'iniezione in-place non ha MAI potuto funzionare su
questo file; i test reali dell'utente lo hanno semplicemente dimostrato.

## (b) Perché `--allow-partial-localization` non lasciava proseguire

**Perché il gate che il flag apriva era a valle del punto di rottura.**
`--allow-partial-localization` bypassava solo il rifiuto dentro
`NeutralizeMuiResource` (step successivo), mentre il fallimento avveniva
**prima**, dentro `InjectCatalogStrings`, che non aveva alcun gate:

```
download ok → identità ok → patch import ok → scrittura copia ok
   → InjectCatalogStrings  →  FAILED (UpdateResource, GetLastError=50)
              ^ qui si fermava SEMPRE
   → NeutralizeMuiResource (il gate del flag) ... MAI RAGGIUNTO
```

Quindi il flag era **cosmetico per questo fallimento**: modificava una
decisione che il programma non raggiungeva mai. Conferma dai 3 log utente
(online / `--offline` / `--allow-partial-localization`): messaggio identico,
stesso punto.

Per questo in v0.0.3 il flag **non esiste più**: la politica è STOP NETTO
su qualunque errore di generazione/validazione/iniezione (`exit 1` con la
transazione risorse scartata). Niente opzioni "parziali" non funzionanti.

## Bug autonomo trovato durante i lavori (già corretto)

`tools/analyze_mui.py::parse_string_table` assegnava gli ID stringa con
off-by-one (slot `i` → `base+i` invece di `base+i+1`). Tutti i 161 ID di
`explorer.exe.constraints.json` (generati col tool buggy) e i corrispondenti
testi catalogo sono stati slittati **+1** ai veri ID Win32 (`base+i+1`);
i vincoli shell32 (5381/5382/5384/5385, origine `StartMenuPin.cpp`) erano e
restano corretti. Test dedicato in `tests/test_build_resources.py`.
