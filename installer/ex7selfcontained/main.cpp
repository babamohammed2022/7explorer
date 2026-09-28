// main.cpp — ex7selfcontained: download-once, verify, patch, localize.
//
// Pipeline (every step fails LOUDLY, never silently, never blocks logon):
//   0. parse args, set up directories + readable log;
//   1. reuse the hash-verified cached explorer.exe when present (OFFLINE OK);
//      otherwise download it from the pinned Microsoft symbol server URL
//      with timeouts/deadline/cancellation, temp->hash->MoveFileEx;
//   2. structural PE identity check (AMD64, TimeDateStamp, SizeOfImage);
//   3. optional secondary Authenticode check on the PRISTINE file
//      (advisory; the signature is inevitably broken by ANY later patch);
//   4. copy to the working explorer.exe, run the deterministic import patch
//      (SHLWAPI.DLL/OLE32.DLL/EXPLORERFRAME.DLL -> wrp64.dll);
//   5. inject the PROJECT-GENERATED resource payloads (all catalog
//      languages, STRINGTABLE/MENU/DIALOGEX/ACCELERATOR, embedded as
//      validated blobs in lang_catalog.h) and neutralize the "MUI" resource
//      (MUI -> CUI) so no external explorer.exe.mui is ever sought;
//   6. refresh the PE CheckSum and record the FINAL post-localization
//      SHA-256 in state\install.json so a corrupted local copy is detected
//      on later runs. Any localization failure aborts (exit 1): the update
//      transaction is discarded, no half-localized shell is ever produced.
//
// BUILD STATUS: syntax-reviewed only (sandbox has no Windows toolchain);
// see README.md for build commands.
#include "config.h"
#include "downloader.h"
#include "winhash.h"
#include "importpatch.h"
#include "localizer.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::wstring g_appDir;
HANDLE g_cancelEvent = nullptr;

void FileLog(const wchar_t* fmt, ...) {
    wchar_t msg[1600];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t line[2000];
    SYSTEMTIME st;
    GetSystemTime(&st);
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"[%04u-%02u-%02u %02u:%02u:%02uZ] %s\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, msg);
    wprintf(L"%s", line);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        // log is UTF-16 to stay readable with non-Latin catalogs
        DWORD wr = 0;
        WriteFile(g_logFile, line, (DWORD)wcslen(line) * 2, &wr, nullptr);
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ev) {
    switch (ev) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_cancelEvent) SetEvent(g_cancelEvent);
        return TRUE;
    }
    return FALSE;
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

bool WriteInstallRecord(const std::wstring& stateDir,
                        const std::wstring& originalHash,
                        const std::wstring& patchedHash,
                        const std::vector<std::string>& actions) {
    std::wstring file = Join(stateDir, L"install.json");
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\\' || c == '"') o += '\\';
            o += c;
        }
        return o;
    };
    std::string json = "{\n  \"tool\": \"ex7selfcontained\",\n  \"version\": \"";
    json += std::string(cfg::kToolVersion, cfg::kToolVersion +
                        wcslen(cfg::kToolVersion));
    json += "\",\n  \"originalSha256\": \"";
    json += std::string(originalHash.begin(), originalHash.end());
    json += "\",\n  \"patchedSha256\": \"";
    json += std::string(patchedHash.begin(), patchedHash.end());
    json += "\",\n  \"actions\": [\n";
    for (size_t i = 0; i < actions.size(); ++i) {
        json += "    \"" + esc(actions[i]) + "\"";
        json += (i + 1 < actions.size()) ? ",\n" : "\n";
    }
    json += "  ]\n}\n";
    DWORD wr = 0;
    WriteFile(h, json.data(), (DWORD)json.size(), &wr, nullptr);
    CloseHandle(h);
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = ex7::OpenForReadShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0, total = 0;
    while (total < out.size()) {
        DWORD chunk = (out.size() - total) > ((size_t)1 << 24)
                          ? (1u << 24)
                          : (DWORD)(out.size() - total);
        if (!ReadFile(h, out.data() + total, chunk, &rd, nullptr) || !rd)
            break;
        total += rd;
    }
    CloseHandle(h);
    return total == out.size();
}

bool WriteWholeFile(const std::wstring& path,
                    const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0, total = 0;
    while (total < data.size()) {
        DWORD chunk = (data.size() - total) > ((size_t)1 << 24)
                          ? (1u << 24)
                          : (DWORD)(data.size() - total);
        if (!WriteFile(h, data.data() + total, chunk, &wr, nullptr) || !wr)
            break;
        total += wr;
    }
    CloseHandle(h);
    return total == data.size();
}

int Usage() {
    wprintf(L"ex7selfcontained [--app-dir PATH] [--offline] "
            L"[--skip-signature]\n"
            L"ex7selfcontained --selftest-importpatch <original.exe>\n"
            L"    writes <original.exe>.ex7patched and prints its SHA-256;\n"
            L"    the CI compares it byte-for-byte with tools/patch_imports.py\n"
            L"\nLocalization resources are GENERATED by the project and embedded in\n"
            L"this binary (localization/catalog + templates -> lang_catalog.h);\n"
            L"no external .mui file is needed, supplied or read. Any resource\n"
            L"build or injection failure aborts the setup (no partial shells).\n");
    return 2;
}

// Deterministic self-test used by CI: the C++ import patch must produce the
// exact same bytes as the Python reference on the same input file.
int SelfTestImportPatch(const std::wstring& input) {
    std::vector<uint8_t> image;
    if (!ReadWholeFile(input, image)) {
        wprintf(L"selftest: cannot read %s\n", input.c_str());
        return 1;
    }
    auto r = ex7::PatchImportsInPlace(image);
    if (!r.ok) {
        wprintf(L"selftest: patch failed\n");
        return 1;
    }
    for (const auto& a : r.actions)
        wprintf(L"selftest: %S\n", a.c_str());
    // idempotency check
    {
        std::vector<uint8_t> again = image;
        auto r2 = ex7::PatchImportsInPlace(again);
        if (!r2.ok || again != image || r2.namesPatched != 0) {
            wprintf(L"selftest: NOT idempotent\n");
            return 1;
        }
        wprintf(L"selftest: idempotent: OK\n");
    }
    std::wstring out = input + L".ex7patched";
    if (!WriteWholeFile(out, image)) {
        wprintf(L"selftest: cannot write output\n");
        return 1;
    }
    HANDLE h = ex7::OpenForReadShared(out);
    std::wstring hash = ex7::Sha256HexOfHandle(h);
    CloseHandle(h);
    wprintf(L"selftest: wrote %s\nselftest: sha256 %s\n",
            out.c_str(), hash.c_str());
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool offline = false, skipSig = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--offline") offline = true;
        else if (a == L"--skip-signature") skipSig = true;
        else if (a == L"--app-dir" && i + 1 < argc) g_appDir = argv[++i];
        else if (a == L"--selftest-importpatch" && i + 1 < argc)
            return SelfTestImportPatch(argv[++i]);
        else return Usage();
    }
    if (g_appDir.empty()) {
        wchar_t self[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, self, (DWORD)_countof(self));
        std::wstring p = self;
        size_t bs = p.find_last_of(L'\\');
        g_appDir = (bs == std::wstring::npos) ? L"." : p.substr(0, bs);
    }
    std::wstring logDir = Join(g_appDir, cfg::kLogSubDir);
    CreateDirectoryW(Join(g_appDir, cfg::kStateSubDir).c_str(), nullptr);
    CreateDirectoryW(logDir.c_str(), nullptr);
    ex7::g_log = FileLog;
    std::wstring logPath = Join(logDir, cfg::kLogFileName);
    g_logFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        // UTF-16 BOM once (when the file is new)
        if (SetFilePointer(g_logFile, 0, nullptr, FILE_END) == 0) {
            WORD bom = 0xFEFF;
            DWORD wr;
            WriteFile(g_logFile, &bom, 2, &wr, nullptr);
        }
    }

    g_cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    FileLog(L"ex7selfcontained %s starting, appDir=%s", cfg::kToolVersion,
            g_appDir.c_str());

    // ---- 1. pristine copy (offline reuse or verified download) -----------
    ex7::DownloadOptions dl;
    dl.appDir = g_appDir;
    dl.cancelEvent = g_cancelEvent;
    std::wstring pristine;
    if (offline)
        FileLog(L"--offline: network disabled for this run");
    if (!EnsurePristineExplorer(dl, pristine)) {
        FileLog(L"FAILED: no verified explorer.exe available; nothing installed");
        return 1;
    }

    // ---- 2. PE identity (incl. exact byte size, confirmed 2026-09-28) ----
    std::wstring diag;
    if (!ex7::CheckPeIdentity(pristine, 512 * 1024, cfg::kExpectedFileBytes,
                              cfg::kTimeDateStamp, cfg::kSizeOfImage, diag)) {
        FileLog(L"FAILED: PE identity check: %s", diag.c_str());
        return 1;
    }

    // ---- 3. Authenticode: SECONDARY, advisory only -----------------------
    // Measured on the real files (2026-09-28): the symbol-server copy is
    // catalog-signed at OS level and the served PE has NO embedded
    // signature (WinVerifyTrust => NotSigned is EXPECTED here). The primary
    // controls are the exact structure + the pinned SHA-256 allow-list.
    // A valid embedded signature, when present, is logged as a bonus.
    if (!skipSig) {
        auto ts = ex7::CheckAuthenticode(pristine, diag);
        FileLog(L"authenticode(pristine, advisory): %s", diag.c_str());
        if (ts == ex7::TrustStatus::Valid)
            FileLog(L"embedded signature present and valid (bonus)");
        else
            FileLog(L"no embedded signature — EXPECTED for symbol-server "
                    L"copies; continuing (structure + SHA-256 allow-list "
                    L"are the primary controls)");
    }

    // ---- 4. working copy: patch imports ----------------------------------
    std::wstring workPath = Join(g_appDir, L"explorer.exe");
    std::vector<uint8_t> image;
    if (!ReadWholeFile(pristine, image)) {
        FileLog(L"FAILED: cannot read pristine file");
        return 1;
    }
    auto patch = ex7::PatchImportsInPlace(image);
    if (!patch.ok) {
        FileLog(L"FAILED: import patch error");
        return 1;
    }
    if (patch.namesPatched == 0) {
        FileLog(L"NOTE: import patch already applied (idempotent)");
    }
    for (const auto& a : patch.actions)
        FileLog(L"patch: %S", a.c_str());
    // policy: SHLWAPI + OLE32 must always have been present in the binary
    if (patch.namesPatched != 0 && patch.namesPatched < 2) {
        FileLog(L"FAILED: expected at least 2 import rewrites, got %u",
                patch.namesPatched);
        return 1;
    }

    // ---- 5. working copy on disk ------------------------------------------
    if (!WriteWholeFile(workPath, image)) {
        FileLog(L"FAILED: cannot write working copy");
        return 1;
    }

    // ---- 6. localization (PROJECT-GENERATED resources, strict stop) -------
    // Full atomic resource-table rewrite (see localizer.h for the CI-proven
    // root cause: in-place UpdateResource into the MU-marked LN binary is
    // refused with ERROR_NOT_SUPPORTED). Every existing resource is copied
    // verbatim, "MUI" becomes "CUI", and the project-generated payloads
    // (en-US fallback + it-IT) are committed in one transaction. Any failure
    // aborts with a hard error and the transaction discarded — the copy is
    // never left half-localized. NO external .mui is ever read or required.
    std::wstring err;
    if (!ex7::LocalizeWithGeneratedResources(workPath, err)) {
        FileLog(L"FAILED: localization generation/injection FAILED: %s",
                err.c_str());
        return 1;
    }
    if (!ex7::RefreshPeChecksum(workPath, err)) {
        FileLog(L"FAILED: PE checksum refresh: %s", err.c_str());
        return 1;
    }

    // ---- 7. final hash + install record (post-localization image) ---------
    std::wstring patchedHash;
    {
        HANDLE h = ex7::OpenForReadShared(workPath);
        patchedHash = ex7::Sha256HexOfHandle(h);
        CloseHandle(h);
    }
    {
        HANDLE h = ex7::OpenForReadShared(pristine);
        std::wstring origHash = ex7::Sha256HexOfHandle(h);
        CloseHandle(h);
        WriteInstallRecord(Join(g_appDir, cfg::kStateSubDir), origHash,
                           patchedHash, patch.actions);
    }
    FileLog(L"localized explorer.exe sha256=%s", patchedHash.c_str());

    FileLog(L"DONE. working copy ready at %s", workPath.c_str());
    return 0;
}
