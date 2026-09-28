// localizer.h — on-machine generation of the language resources.
//
// Strategy chosen in docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md (option b):
//  1. the private, already hash-verified + import-patched explorer.exe copy
//     gets its "MUI" resource NEUTRALIZED (type renamed "MUI" -> "CUI"), so
//     the kernel MUI loader stops looking for an external explorer.exe.mui
//     (no checksum pairing to satisfy);
//  2. the embedded catalog (lang_catalog.h, GENERATED from
//     localization/catalog/*.json) is injected as per-language STRINGTABLE
//     resources with UpdateResource;
//  3. shell32 needs NOTHING at runtime: its four strings (ids 5381/5382/
//     5384/5385, verified in explorerwrapper/StartMenuPin.cpp) are served by
//     LoadStringW(g_hInstance) fallback inside wrp64.dll, which embeds all
//     catalog languages at build time (explorerwrapper/ex7_languages.rc).
//
// SAFETY GATE: the neutralization (step 1) is only performed when the
// catalog declares coverage of every STRINGTABLE id the reference
// explorer.exe.mui contains; until
// localization/constraints/explorer.exe.constraints.json exists, pass
// allowPartial == false and step 1 is REFUSED (no half-localized shell).
#pragma once

#include <string>
#include <vector>

namespace ex7 {

struct LocalizeOptions {
    // STRINGTABLE ids the catalog currently covers; when explorer-exe
    // constraints land this list comes from lang_catalog.h generation.
    bool forceAllowPartial = false;  // explicit user override, logged loudly
};

// Builds the 16-entry STRINGTABLE block payload (length-prefixed UTF-16)
// for block id `blockId` (covering ids blockId*16-16+1 .. blockId*16).
std::vector<unsigned char> BuildStringBlockPayload(
    unsigned int blockId,
    const struct Ex7LangStringEntry* entries, unsigned int count);

// Injects every catalog language's STRINGTABLE entries into `exePath`.
// Returns false with `error` set on failure (file left untouched when the
// final EndUpdateResource is not committed — the OS guarantees discard).
bool InjectCatalogStrings(const std::wstring& exePath, std::wstring& error);

// Renames the "MUI" RCDATA type to "CUI" inside `exePath`.
// Requires the coverage gate (see header comment and LocalizeOptions).
bool NeutralizeMuiResource(const std::wstring& exePath,
                           const LocalizeOptions& opt,
                           std::wstring& error);

} // namespace ex7
