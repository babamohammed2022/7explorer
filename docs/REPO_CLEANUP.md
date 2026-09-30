# Repository cleanup (2026-09-29)

This document describes the repository reorganization performed in the chore commit `chore(repo): cleanup` (branch `arena/01a0edc7-7explorer`, PR targeting `main`). The resulting state was: **`main` plus one active working branch**, no build artifacts in the repository, and **one reference release**.

## Branches

### Starting point

There were 405 branches on `origin` at the time of cleanup:

| Category | Count | Contents |
|---|---:|---|
| `ci-logs/*` (`sc-`, `locpipe-`, `rel-`, `theme-`, `updres-`, `w81-`, `diag`, `diagloc`, `exit-`, `pnidui-`, `tray-`) | 401 | CI output only: build logs, `release.txt`, `pipe-upload/`, diagnostic dumps. 1–2 commits each, no source code (verified with `git log main..<branch>` and `git diff --stat` on a significant sample). |
| `arena/01a0e6b6-7explorer` | 1 | Agent session—**already merged** into main through PR #1 (0 commits not in main). |
| `arena/01a0e9d8-7explorer` | 1 | Agent session—**already merged** into main through PR #2 (0 commits not in main). |
| `arena/01a0edc7-7explorer` | 1 | Active agent session (PR #3). |
| `main` | 1 | Project history. |

### What was done

- **Deleted 401 `ci-logs/*` branches**: they contained build logs only, not source code. CI now publishes logs as run **artifacts** (see below), so the branches are no longer needed. No useful work was lost: for every branch, `git rev-list --count main..<branch>` was checked to confirm that the additional commits contained only logs (`.txt`/`.log` files). The initial count was 397; the last four CI runs before the workflow fix created four more, also containing logs only.
- **Deleted 2 already-merged `arena/*` branches** (PR #1 and PR #2; their history remains in `main` through the merge commits).
- **Remaining**: `main` plus `arena/01a0edc7-7explorer` (the active working branch for that session and source of the PR; it can be deleted after merge).

The verification method, which can be repeated:

```
git fetch origin '+refs/heads/*:refs/remotes/origin/*' --prune
for b in $(git branch -r | sed 's| *origin/||' | grep -v '^HEAD$'); do
  echo "$(git rev-list --count origin/main..origin/$b) $b"
done | sort -rn
```

No force-push to `main` was performed. No branch with unique source work was deleted.

## CI logs: from branches to artifacts

Previously, the `selfcontained-ci`, `localization-pipeline`, and seven `diag-*` workflows committed build logs to `ci-logs/<prefix>-<run id>` branches (about 400 accumulated branches). Now:

- all run logs are uploaded as **artifacts** of the run itself (`actions/upload-artifact@v4`) and remain available even after failures, with 30-day retention;
- the "Commit build/release/logs" steps were removed from all workflows;
- `permissions` were reduced to `contents: read` for workflows that do not publish releases (`selfcontained-ci` retains `contents: write` only for the tag prerelease job);
- the `ci-logs/` directory was removed from the repository (it contained only a `.gitkeep`), and CI working directories (`ci-logs/`, `logs/`, `ci-run/`, `collect-logs/`, `exp/`, `work/`, etc.) are now in `.gitignore`.

## Artifacts removed from the repository

| File | Reason |
|---|---|
| `ci-logs/` (directory, `.gitkeep` only) | Placeholder for build logs; not source code. |
| `explorerwrapper/explorerwrapper.vcxproj.user` | Visual Studio `.user` file (local developer settings). |
| `explorerwrapper/libMinHook.x64.lib` (520 KB) | MinHook build output (third-party static library). CI clones and builds MinHook on each run (`msbuild.yml`, `selfcontained-ci.yml`); for local builds, see the README (**MinHook**). Added to `.gitignore`. |
| `localization/explorer.exe.mui` (22 KB) | Microsoft binary (Win7 `explorer.exe` `.mui` file), used only as a test reference. Tests already use the by-design fallback `tests/fixtures/win7_explorer_structure.json` (structure without Microsoft text, same `source_sha256`). Verified: `python tests/run_tests.py` → 51 tests pass even without this file. To compare with the real file, set `WIN7EXPLORERRESTORER_REF_MUI=<path>`. |

Post-removal verification: the full Python suite passed (51 tests), and workflow YAML files were validated.

## Releases

### Starting point

There were 39 releases (all prereleases): `v0.0.1-test1` … `v0.0.3-test36`, one for each test iteration, plus the new `v0.3-test37`. Each release contained the same assets (`wrp64.dll`, `Win7ExplorerRestorer.exe`, `shell-switcher.exe`, `Win7ExplorerRestorer-test-bundle.zip`, Windhawk sources, and `SHA256SUMS.txt`). The 38 old tags had 38 matching old releases (verified with `git ls-remote --tags` and the `/releases` API; no orphan tags).

### What was done

- **New reference release: `v0.3-test37`** (tagged on the PR merge to `main`, built by CI from the exact tag):
  - body = `docs/RELEASE_NOTES_test.md`: what works, known issues, downloads, and a table of files downloaded at runtime with their URLs and SHA-256 hashes;
  - assets: `Win7ExplorerRestorer-test-bundle.zip` (complete bundle corresponding to the tag), individual binaries, and `SHA256SUMS.txt`.
- **The 38 previous releases were deleted**, along with their tags: they were overlapping test snapshots and were not linked from documentation. The source for every release remains in Git history.
- Of the 39 total tags, **one remains**: `v0.3-test37`.

Tag name `v0.3-test37`: `0.3` represents maturation of the `0.0.x` series (self-contained installation + switcher + logon fix); `test37` continues the `testNN` numbering in `main` (the latest on main was test36).

### Test38 update (release policy for every release)

- Release notes must be GENERIC and in English, using the fixed schema in `docs/RELEASE_NOTES_test.md` (the same schema as `0.0.3-alpha`): never say what was improved, fixed, or remains incomplete.
- The only downloadable asset is `Win7ExplorerRestorer-test-bundle.zip` (enforced by the `prerelease` job in `.github/workflows/selfcontained-ci.yml`).

## Final state

- Branches: `main` + `arena/01a0edc7-7explorer` (active work; the name indicates the Arena session)
- Tag: `v0.3-test37`
- Releases: 1 (reference prerelease)
- No binaries or build artifacts in the repository
- CI: logs stored as artifacts, minimum permissions, green workflows
