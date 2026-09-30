# Root cause — v0.0.2-test1: "UpdateResource failed for a string block"

*(Analysis completed 2026-09-28. Before v0.0.3 was implemented, the user asked (a) why `UpdateResource` failed and (b) why `--allow-partial-localization` still did not let the installer continue.)*

## (a) Why "UpdateResource failed for a string block"

**Proven by CI: the file is a language-neutral PE marked MU (RCDATA "MUI"), and `UpdateResourceW` REFUSES to add resources to this type of binary, returning `ERROR_NOT_SUPPORTED (50)`.** This was not caused by the payload, language, alignment, or permissions; Windows structurally rejects updates to MU-linked PE files.

Evidence 1 — reproducing the user's run in CI (`ci-logs/diagloc-36412428950`, `windows-latest` run with instrumented code): the same failure occurred, with the error now visible:

```
FAILED: localization injection: UpdateResource failed for string block 337
(lcid 0x0407, 376 bytes), GetLastError=50
```

Evidence 2 — dedicated probe `ci/updres/Program.cs` (results in `ci-logs/updres-*`, e.g. run 36415198307), against the actual Microsoft file:

| Variant | Result |
|---|---|
| V1: write en-US strings (0x0409) to the file as-is | `ok=False gle=50` ❌ |
| V2: write de-DE strings (0x0407) | `ok=False gle=50` ❌ (therefore, not language-dependent) |
| V3a: delete the "MUI" type using `UpdateResource(...,NULL,0)` | `ok=False gle=87` ❌ (`ERROR_INVALID_PARAMETER`) |
| V4: delete MUI and write in the same transaction | delete MUI `gle=87`, write `gle=1359` ❌ |
| **V5: `BeginUpdateResource(bDeleteExistingResources=TRUE)` + rewrite everything** | **ok ✅** |

Conclusion: the only approach Windows accepts for this binary is an **atomic rewrite of the entire resource table**—exactly what v0.0.3 does (`LocalizeWithGeneratedResources`: enumerate everything, copy everything, omit "MUI" (renamed to "CUI"), write the generated blobs, and commit one transaction).

Historical consequence: in-place injection could NEVER work on this file; the user's real-world tests simply demonstrated it.

## (b) Why `--allow-partial-localization` did not let the installer continue

**Because the gate controlled by the flag was downstream from the failure.** `--allow-partial-localization` bypassed only the rejection inside the later `NeutralizeMuiResource` step, while the failure occurred **earlier**, inside `InjectCatalogStrings`, which had no gate:

```
download ok → identity ok → import patch ok → copy written ok
   → InjectCatalogStrings  →  FAILED (UpdateResource, GetLastError=50)
              ^ it always stopped here
   → NeutralizeMuiResource (the flag's gate) ... NEVER REACHED
```

The flag was therefore **cosmetic for this failure**: it changed a decision that the program never reached. This was confirmed by the three user logs (online / `--offline` / `--allow-partial-localization`): identical message, identical failure point.

That is why the flag **no longer exists** in v0.0.3: the policy is to STOP on any resource-generation/validation/injection error (`exit 1`, with the resource transaction discarded). There are no nonfunctional "partial" options.

## Independent bug found during the work (already fixed)

`tools/analyze_mui.py::parse_string_table` assigned string IDs with an off-by-one error (slot `i` → `base+i` instead of `base+i+1`). All 161 IDs in `explorer.exe.constraints.json` (generated with the buggy tool) and their corresponding catalog texts were shifted **+1** to the actual Win32 IDs (`base+i+1`). The shell32 constraints (5381/5382/5384/5385, from `StartMenuPin.cpp`) were and remain correct. A dedicated test is in `tests/test_build_resources.py`.
