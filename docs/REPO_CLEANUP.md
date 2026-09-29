# Pulizia del repository (2026-09-29)

Questo documento descrive la riorganizzazione del repository eseguita con
il chore commit `chore(repo): cleanup` (branch `arena/01a0edc7-7explorer`,
PR verso `main`). Lo stato finale è: **`main` + un solo branch di lavoro
attivo**, nessun artefatto di build nel repository, **una sola release di
riferimento**.

## Branch

### Situazione di partenza

405 branch su `origin` al momento della pulizia:

| categoria | numero | contenuto |
|---|---|---|
| `ci-logs/*` (`sc-`, `locpipe-`, `rel-`, `theme-`, `updres-`, `w81-`, `diag`, `diagloc`, `exit-`, `pnidui-`, `tray-`) | 401 | solo output di CI: log di build, `release.txt`, `pipe-upload/`, dump diagnostici. 1–2 commit ciascuno, zero sorgente (verificato con `git log main..<branch>` e `git diff --stat` su un campione significativo) |
| `arena/01a0e6b6-7explorer` | 1 | sessione agente — **già mergiata** in main via PR #1 (0 commit non in main) |
| `arena/01a0e9d8-7explorer` | 1 | sessione agente — **già mergiata** in main via PR #2 (0 commit non in main) |
| `arena/01a0edc7-7explorer` | 1 | sessione agente attiva (PR #3) |
| `main` | 1 | storia del progetto |

### Cosa è stato fatto

- **401 branch `ci-logs/*` eliminati**: contenevano solo log di build, non
  sorgente. La CI ora pubblica i log come **artifact** della run (vedi
  sotto), quindi i branch non servono più. Nessun lavoro utile perso: per
  ognuno è stato verificato con `git rev-list --count main..<branch>` che i
  commit aggiuntivi fossero solo i log (file `.txt`/.log). Nota: al primo
  censimento erano 397; le ultime run CI precedenti al fix del workflow
  ne avevano creati altri 4, tutti comunque di soli log.
- **2 branch `arena/*` già mergiati eliminati** (PR #1 e PR #2: la loro
  storia resta in `main` tramite i merge commit).
- **Rimane**: `main` + `arena/01a0edc7-7explorer` (branch di lavoro attivo
  di questa sessione, da cui parte il PR; eliminabile dopo il merge).

Metodo usato per la verifica, ripetibile:

```
git fetch origin '+refs/heads/*:refs/remotes/origin/*' --prune
for b in $(git branch -r | sed 's| *origin/||' | grep -v '^HEAD$'); do
  echo "$(git rev-list --count origin/main..origin/$b) $b"
done | sort -rn
```

Nessun force-push su `main`. Nessun branch con lavoro sorgente unico è stato
eliminato.

## Log di CI: da branch ad artifact

Prima: i workflow `selfcontained-ci`, `localization-pipeline` e i sette
`diag-*` committavano i log di build su branch `ci-logs/<prefisso>-<run id>`
(~400 branch accumulati). Ora:

- tutti i log di run vengono caricati come **artifact** della run stessa
  (`actions/upload-artifact@v4`), sempre disponibili anche in caso di
  fallimento, con retention a 30 giorni;
- gli step "Commit build/release/logs" sono stati rimossi da tutti i
  workflow;
- `permissions` ridotte a `contents: read` per tutti i workflow che non
  pubblicano release (`selfcontained-ci` mantiene `contents: write` solo
  per il job prerelease sui tag);
- la cartella `ci-logs/` è stata rimossa dal repository (conteneva solo un
  `.gitkeep`) e le directory di lavoro della CI (`ci-logs/`, `logs/`,
  `ci-run/`, `collect-logs/`, `exp/`, `work/`, …) sono ora in `.gitignore`.

## Artefatti rimossi dal repository

| file | motivo |
|---|---|
| `ci-logs/` (dir, solo `.gitkeep`) | segnaposto per log di build: non è sorgente |
| `explorerwrapper/explorerwrapper.vcxproj.user` | file `.user` di Visual Studio (impostazioni locali sviluppatore) |
| `explorerwrapper/libMinHook.x64.lib` (520 KB) | output di build di MinHook (libreria statica di terze parti). La CI clona e compila MinHook a ogni run (`msbuild.yml`, `selfcontained-ci.yml`); per build locali vedi il README ("Minhook"). Aggiunto a `.gitignore` |
| `localization/explorer.exe.mui` (22 KB) | binario Microsoft (`.mui` di explorer.exe Win7) usato solo come riferimento dai test. I test usano già il fallback by-design `tests/fixtures/win7_explorer_structure.json` (struttura senza testo Microsoft, stesso `source_sha256`); verificato: `python tests/run_tests.py` → 51 test OK anche senza il file. Chi vuole il confronto col file reale può passare `WIN7EXPLORERRESTORER_REF_MUI=<percorso>` |

Verifica post-rimozione: suite Python completa OK (51 test), workflow YAML
validati.

## Release

### Situazione di partenza

39 release (tutte pre-release): `v0.0.1-test1` … `v0.0.3-test36`, una per
ogni iterazione di test, più la nuova `v0.3-test37`. Ogni release conteneva
gli stessi asset (wrp64.dll, Win7ExplorerRestorer.exe,
7explorer-shell-switcher.exe, Win7ExplorerRestorer-test-bundle.zip, sorgenti windhawk,
SHA256SUMS.txt). A 38 vecchi tag corrispondevano 38 vecchie release
(verificato con `git ls-remote --tags` e l'API `/releases`: nessun tag
orfano).

### Cosa è stato fatto

- **Nuova release di riferimento: `v0.3-test37`** (tag sul merge del PR in
  `main`, costruita dalla CI dal tag esatto):
  - corpo = `docs/RELEASE_NOTES_test.md`: cosa funziona, problemi noti,
    download, tabella dei file scaricati a runtime con URL + SHA-256;
  - asset: `Win7ExplorerRestorer-test-bundle.zip` (bundle completo corrispondente al tag) +
    binari singoli + `SHA256SUMS.txt`.
- **Le 38 release precedenti sono state eliminate** insieme ai rispettivi
  tag: erano istantanee di test sovrapposte, nessuna indicizzata da
  documentazione. La sorgente di ogni release resta nella storia git.
- Dei 39 tag complessivi ne resta **uno**: `v0.3-test37`.

Tag nominativo `v0.3-test37`: `0.3` = maturazione della serie `0.0.x`
(self-contained + switcher + fix logon), `test37` = prosecuzione diretta
della numerazione `testNN` della storia di main (ultimo in main: test36).

## Stato finale

- branch: `main` + `arena/01a0edc7-7explorer` (lavoro attivo, motivo nel
  nome: sessione Arena)
- tag: `v0.3-test37`
- release: 1 (di riferimento, pre-release)
- niente binari né artefatti di build nel repository
- CI: log come artifact, permessi minimi, workflow verdi
