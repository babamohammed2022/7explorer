# Plan: Fully self-contained installation of Windows 7 Explorer Restorer

> Note: **historical/technical document** (2026-09-28)—this is the installer design plan, kept for reference. The current user installation status is documented in [installazione.md](installazione.md).

Analysis date: 2026-09-28. Status legend for each claim:

- ✅ **VERIFIED IN SOURCE** — read directly in this repository's code (file:line citation).
- 🧪 **VERIFIED IN SANDBOX** — tested by the agent (24/24 passing when this was written: `python3 tests/run_tests.py`).
- 🌐 **KNOWN EXTERNAL SOURCE** — cannot be rechecked from the sandbox.
- ⚠️ **MUST BE VERIFIED ON THE USER'S MACHINE** — the sandbox cannot access `msdl.microsoft.com` (TLS reset; GitHub is reachable). Ready-to-run commands are included below.

---

## 1. Downloading and verifying explorer.exe

Values supplied by the user:

| Value | Constant | Status |
|---|---|---|
| TimeDateStamp `0x4CE7A144` | `cfg::kTimeDateStamp` | ✅ **CONFIRMED** by user (Win10 19044) and CI (header dump of the actual file) |
| SizeOfImage `0x2C0000` | `cfg::kSizeOfImage` | ✅ **CONFIRMED** by user and CI |
| File size 2,872,320 bytes | `cfg::kExpectedFileBytes` | ✅ **CONFIRMED** by user and CI (exact check) |
| SHA-256 `5769…e21b` | `cfg::kAcceptedSha256[0]` | ✅ observed by user on 2026-09-28 (`certutil`) |
| SHA-256 `6a671b…7576a` | `cfg::kAcceptedSha256[1]` | ✅ observed by CI on 2026-09-28 (Azure, 2 runs, 3 user agents) |
| URL `…/explorer.exe/4CE7A1442C0000/explorer.exe` | template | ✅ supplied by user and CI |

**A variant of the same binary was found**: the symbol server serves at least two **structurally identical** copies of the same build (same machine/TimeDateStamp/SizeOfImage/size/import list—verified against the actual file in CI) with **different SHA-256 hashes**. The likely explanation is re-signing/retimestamping of the same contents (the difference does not affect code or headers). The verification model was updated to require **exact structure + documented SHA-256 allow-list + Authenticode by default**. Never accept files outside the list; extend it only after documented observation, as above.

**Verification commands to run on your machine before release**, with the expected output to paste/check:

```bat
curl.exe -L -o %TEMP%\explorer-ref.exe "https://msdl.microsoft.com/download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe"
certutil -hashfile %TEMP%\explorer-ref.exe SHA256
python tools\analyze_mui.py %TEMP%\explorer-ref.exe --dump-headers
```

`certutil` must print the expected hash exactly. `--dump-headers` prints TimeDateStamp/SizeOfImage/machine, whether the `MUI` resource exists, and the import list. This also confirms which of `SHLWAPI.DLL`, `OLE32.DLL`, and `EXPLORERFRAME.DLL` are actually present (`EXPLORERFRAME` is conditional; see task 2).

Implemented in `installer/Win7ExplorerRestorer/downloader.cpp` (WinInet with per-phase timeouts, 120-second overall deadline, immediate cancellation at logoff/shutdown via `SetConsoleCtrlHandler`, 3 retries with backoff, 16 MB cap, temporary file → hash check → `MoveFileEx`) and `winhash.cpp` (CNG SHA-256 using the already-open handle with `FILE_SHARE_READ|FILE_SHARE_DELETE`; PE identity check: AMD64, PE32+, TimeDateStamp, SizeOfImage; `WinVerifyTrust` as a non-blocking secondary check on the *pristine* file—the signature is inevitably invalidated by any patch, ✅ a logical consequence).

The **original file's hash is always checked BEFORE patching**. The hash of the **patched copy is stored in `state\install.json`** on every install (to detect corrupted local copies on restart: the `.pris` cache is re-verified on every run—✅ logic implemented in `EnsurePristineExplorer`).

Any file that is not an exact match is rejected: there is no "try anyway" fallback, the log explicitly reports `HASH MISMATCH`, and the file is deleted.

## 2. Patching imports (no more CFF Explorer)

✅ **VERIFIED IN SOURCE** (`README.md`, "Step 2 - Patching explorer.exe"): users currently have to replace the `SHLWAPI.DLL`, `OLE32.DLL`, and (if present) `EXPLORERFRAME.DLL` imports manually.

Implementation: `tools/patch_imports.py` (cross-platform reference) and its 1:1 port, `installer/Win7ExplorerRestorer/importpatch.cpp`.

**Is it deterministic? Yes 🧪**, demonstrated as follows:

- `tests/test_patch_imports.py` (10 tests) builds synthetic PE32+ files with an import table and verifies: identical output bytes on every run; **idempotency** (re-patching an already patched file makes no changes and produces an empty action log); only the 3 target DLL-name slots plus the CheckSum field change (per-offset diff test); longer names (`EXPLORERFRAME.DLL` 17+1 bytes, `SHLWAPI.DLL` 11+1 bytes) are zero-padded to the original length, so **file size is unchanged**; DLL-name comparison is **case-insensitive** (like the Windows loader); the *bound import* directory is cleared (its names are in a separate table, so clearing the directory is the documented safe approach); the **CheckSum** field is recalculated with the standard MapFileAndCheckSum-family algorithm (test verifies that the stored checksum matches the recalculation); non-PE/non-AMD64 input is rejected.

How to test it on the machine:

```bat
python tools\patch_imports.py explorer-pristine.exe out1.exe
python tools\patch_imports.py explorer-pristine.exe out2.exe
fc /b out1.exe out2.exe          :: identical = deterministic
python tools\patch_imports.py out1.exe out1b.exe   :: "nothing to do" = idempotent
Win7ExplorerRestorer --selftest-importpatch explorer-pristine.exe
   :: (to be implemented on the branch: compare byte-for-byte with Python output)
```

Honest note: the C++ port was **reviewed by inspection but not compiled** (the sandbox has no Windows toolchain); its logic matches the tested Python reference 🧪. A trial cross-build is needed in a later PR.

⚠️ Hypothesis to confirm with the `--dump-headers` command above: the Win7 SP1 binary's `SHLWAPI` also has **ordinal imports** (the wrapper exports by ordinal, ✅ visible in `forwards.h` as `FORWARDO(SHLWAPI,…)`). The patch changes only the DLL name in the descriptors, not the thunks; ordinal imports continue to work through `wrp64.dll` as long as the wrapper exports the same ordinals—✅ consistent with the existing project.

## 3. How explorer.exe.mui / shell32.dll.mui resources are loaded today

✅ **VERIFIED IN SOURCE**:

1. **shell32.dll.mui** — loaded **by the wrapper**, not by the Windows MUI loader. `StartMenuPin.cpp:14-46` (`Shell32_LoadString`): the wrapper hooks the `LoadStringW` import in shell32.dll's IAT (through `api-ms-win-core-libraryloader-l1-2-0.dll`, `StartMenuPin.cpp:241`, `h_shell32`); for IDs `0x1505, 0x1506, 0x1508, 0x1509` only (5381/5382/5384/5385), when `hInstance == shell32`, it loads `<exe dir>\<user preferred language>\shell32.dll.mui` with `LoadLibraryEx(..., LOAD_LIBRARY_AS_DATAFILE)`. If the string is not found, it falls back to `LoadStringW(g_hInstance, ...)`, i.e. **resources in `wrp64.dll` itself**. `wrapper.rc` already contains the four English strings. Context: Start menu "pin/unpin" labels (needed because IDs changed in Windows ≥8; see the README's Windows 8.1 "Customize Start Menu" note too). Therefore, **Windows 7 Explorer Restorer actually uses only these four string types**—no shell32 menus or dialogs.
2. **explorer.exe.mui** — loaded by the **kernel MUI loader**. Win7 `explorer.exe` is a language-neutral PE with an `RCDATA "MUI"` resource; its `LoadString`/`LoadMenu`/`LoadDialog`/`LoadAccelerators` calls go through the resource loader, which searches for `<exe dir>\<language>\explorer.exe.mui`. ✅ This matches the README layout (an `en-US` folder next to `explorer.exe`) and the wrapper's use of `GetUserPreferredUILanguages` to build paths. Parsing MUI with cross-validation of checksums (LN↔MUI) is why option (a) would require a `muirct`-like reimplementation; it was rejected as the primary path (see below). This is domain knowledge, 🌐 not re-tested here.

### Choice: justified hybrid option (b) ✅ — **SUPERSEDED by v0.0.3**

> ⚠️ **Update 2026-09-28 (v0.0.3)** — the strategy below (in-place injection + user-provided reference `.mui`) FAILED in real tests and was replaced. Root cause was proven in CI: `UpdateResource` rejects an LN binary marked MU with `ERROR_NOT_SUPPORTED (50)`; deleting the `MUI` marker fails with `ERROR_INVALID_PARAMETER (87)`. v0.0.3 performs a **complete atomic rewrite of the resource table** (`BeginUpdateResource(TRUE)` after enumerating every resource) using **payloads generated by the project** (`tools/build_resources.py` from `localization/catalog` + `localization/templates`): no `.mui` file is needed anywhere in the pipeline. `--allow-partial-localization` was removed. See `installer/Win7ExplorerRestorer/README.md` and `localizer.h`.

- **explorer.exe (private copy)**: neutralize the `MUI` resource → `CUI` (same trick as your mod B) and **inject** into the binary—*in v0.0.3 this became a complete atomic rewrite of the resource table using project-generated payloads (see the banner above and `localization/ROOT_CAUSE_v0.0.3.md`); the details below are historical.* Inject the catalog's STRINGTABLE resources (`localizer.cpp`, `Begin/Update/EndUpdateResource`). **Safety gate**: neutralization is rejected unless `localization/constraints/explorer.exe.constraints.json` exists (otherwise the shell would be only partially localized); use `--allow-partial-localization` only for explicit tests.
- **shell32**: **no generated files**. `tools/embed_catalog.py` generates `explorerwrapper/Win7ExplorerRestorer_languages.rc` (added to the vcxproj) with STRINGTABLE resources in all catalog languages; the existing fallback in `StartMenuPin.cpp` serves them automatically in the UI language. `Windows 7 Explorer Restorer` can also generate a reduced `shell32.dll.mui` (resource-only PE); the wrapper would load it as a data file without cross-validation (🌐: no MUI checksum is involved on that path). This was left as a future, unnecessary improvement.
- No additional `LoadString` hook for Explorer: after MUI is neutralized and strings are injected, `LoadString` reads directly from the binary.

### IDs actually needed

- ✅ shell32.dll.mui: **5381 5382 5384 5385** (verified in source).
- ⚠️ The complete set for explorer.exe.mui cannot be guessed; it must be extracted from the reference file using `tools/analyze_mui.py`. Commands:

```bat
python tools\analyze_mui.py explorer.exe.mui --constraints localization\constraints\explorer.exe.constraints.json
python tools\analyze_mui.py explorer.exe.mui --with-strings > %TEMP%\ref-strings.json   :: DO NOT commit
```

  The first command (structure only: IDs, lengths, accelerators, placeholders, menu/dialog geometry) is committed; the second (reference text) is not.

## 4. Language content and copyright

Rules implemented and 🧪 tested:

- Repository text consists of **original rewordings** in 10 languages (`localization/catalog/*.json`); no Microsoft text is reproduced. The structure (IDs, types, placeholders, accelerator letters, reference lengths) is outside copyright and comes from the analyzer.
- `tools/verify_catalog.py` (CI-ready; exits 1 on the first error) checks placeholders by number/order/type (`%s`, `%d`, `%1!s!`, `%%`, `%I64u`, `%ls`, `%1`, etc.); **one `&` per string**; **unique accelerators** in coexisting contexts (menus/dialogs); length budget (max 2× or +8 characters versus the reference, with commented overrides only); and **complete English fallback coverage**.
- Current catalog: the 4 shell32 strings × 10 languages (en, it, de, fr, es, pt-BR, pl, ru, ja, zh-CN)—structural checks report 0 errors.
- **No Microsoft binaries in the repository**. The reference `mediaexplorer.exe.mui` file (MediaFire) is **not downloaded or committed**; only its *structure* is included in the constraints JSON.

**Languages to have reviewed by a native speaker first (priority order):**
1. **ja** and **zh-CN** (register and accelerator choices `(&X)` in menus);
2. **ru** (use of «» and register);
3. **de / fr / es** (consistent imperative tone);
4. pt-BR and pl are lower priority; en/it have already been reviewed here.

Regarding the question "what if a choice requires including Microsoft text?": with this architecture, **it is not needed**. The only Microsoft text currently in the repository is inherited from upstream in `wrapper.rc` (the four English strings). Removing it is outside my scope without your decision—if you want, I can replace those with our catalog rewordings too (two RC lines); let me know.

## 5. Robustness

| Requirement | Where | Status |
|---|---|---|
| Never block logon/shell startup | WinInet per-phase timeout + 120-second deadline + cancellation on CTRL_LOGOFF/SHUTDOWN; no user wait | ✅ implemented (cannot be compiled here) |
| No network / offline reuse | `.pris` cache re-verified by hash on every run; `--offline` forces offline mode | ✅ implemented |
| No MAX_PATH limitation | `\\?\` prefix for long paths (`OpenForReadShared`) | ✅ implemented |
| Readable logs | `log\Win7ExplorerRestorerSetup.log` with UTF-16 BOM + stdout | ✅ implemented |
| Do not touch system files | everything under `--app-dir`; no access to `%SystemRoot%` | ✅ by construction |
| No Microsoft binaries in repository | JSON structure + original text only | ✅ |
| Target testing | Windows 10/11: still to run (⚠️ checklist below) | 🧪 logic suite passes here |

### Real-machine test checklist (Win10 and Win11)

1. `certutil`/`--dump-headers` confirm the three constants; if not, stop.
2. Online run: download → hash OK → patch → `install.json` contains both hashes → log has no errors.
3. Run with `--offline` and network disabled: reuses the cache in under 1 second.
4. Cancel during download → abort immediately, no cache file remains.
5. Compare two runs with `fc /b` → byte-identical (through `install.json`, which contains stable hashes).
6. Shell: pin/unpin in the Start menu using the UI language; English fallback for missing languages; check for overly long UI strings (verifier warnings).

## 6. Verification status (honest summary)

- 🧪 Done here: deterministic and idempotent patching; resource analyzer on fixtures; catalog verifier (including negative cases); deterministic header/RC generation (two consecutive runs byte-identical). Not reachable from the sandbox: `msdl.microsoft.com`, MediaFire.
- ✅ Confirmed by the user (2026-09-28, Win10 21H2 LTSC 19044): URL, size 2,872,320 bytes, SHA-256, TimeDateStamp, SizeOfImage.
- 🔄 In CI (`selfcontained-ci.yml`): identity re-check on the actual file, Python↔C++ patch comparison byte-for-byte, resulting imports point to `wrp64.dll`; MSVC builds of `wrp64.dll` and `Win7ExplorerRestorer.exe`; Python tests.
- ⚠️ Still open: list of `explorer.exe.mui` IDs (strings/menus/dialogs/accelerators)—awaiting the user's file; behavioral tests on a real Win10/11 machine; shell integration (out of scope for this stage).
