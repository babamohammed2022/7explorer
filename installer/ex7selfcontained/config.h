// config.h — pinned constants for the self-contained bootstrap.
//
// VERIFICATION STATUS: the Microsoft binary identity values below were
// CONFIRMED by the user on 2026-09-28 (Windows 10 21H2 LTSC 19044) with a
// real download + certutil. The CI re-verifies them on every run on real
// files (job "verify-reference"): URL, byte size, SHA-256, TimeDateStamp,
// SizeOfImage. Do not change them without re-running that verification.
#pragma once

namespace cfg {

// ----- identity of the ONE Microsoft binary we need -----------------------
// explorer.exe 6.1.7601.17514 (win7sp1_rtm.101119-1850), x64.
//
// IDENTITY MODEL (multi-level). Microsoft serves AT LEAST TWO legitimate
// variants of this very same build (re-signed/re-timestamped catalog
// copies): measured structure is IDENTICAL (machine AMD64, TimeDateStamp,
// SizeOfImage, byte size, import list) while SHA-256 differs. Observations:
//   [A] 5769e5b2…e21b — user, 2026-09-28, Windows 10 21H2 LTSC 19044,
//        download + certutil (first observation);
//   [B] 6a671b92…7576a — GitHub Actions runner (Azure), 2026-09-28, two
//        independent runs, three user-agents (second observation).
// Policy: a file is accepted ONLY if (1) structural identity below matches
// EXACTLY AND (2) its SHA-256 is in kAcceptedSha256[] AND (3) Authenticode
// verifies (see main.cpp; --skip-signature exists for offline CI tests).
// NEVER accept a file outside this list. Extend the list only after a
// documented verification (who, where, how) like the ones above.
inline constexpr unsigned int  kTimeDateStamp = 0x4CE7A144;
inline constexpr unsigned int  kSizeOfImage = 0x2C0000;
inline constexpr unsigned long long kExpectedFileBytes = 2872320ULL;
inline constexpr const wchar_t* kAcceptedSha256[] = {
    // [A] user-observed, Win10 21H2 LTSC, certutil, 2026-09-28
    L"5769e5b25c7bfbc20dbfdca2f17b751f6d968e03412705de4a16c99b2626e21b",
    // [B] CI-observed, Azure runner, deterministic, 2026-09-28
    L"6a671b92a69755de6fd063fcbe4ba926d83b49f78c42dbaeed8cdb6bbc57576a",
};
inline constexpr unsigned int kAcceptedSha256Count =
    sizeof(kAcceptedSha256) / sizeof(kAcceptedSha256[0]);

// Host/path are pinned: HTTPS only, fixed host, path derived ONLY from the
// two identity constants above (no user influence, no URL parsing).
// ---------- reference explorer.exe.mui (Win7 RTM en-US, lang 0409) ----------
// User-supplied at install time (offline reuse of a verified copy; the symbol
// server does NOT serve .mui files: probe run 36406420970 proved 404 for
// every candidate key). Identity: exact size + TimeDateStamp + allow-listed
// SHA-256 (same documented-variant model as the .exe).
inline constexpr unsigned __int64 kMuiSize = 22016ULL;
inline constexpr unsigned int  kMuiTimeDateStamp = 0x4A5BC954;
inline constexpr const wchar_t* kMuiAcceptedSha256[] = {
    // [A] 2026-09-28 — copia caricata dall'utente (analisi strutturale locale)
    L"4cc514a7d9afae763cdd21932ee722ae4a787c968bab971c3b1d30044151cfe3",
};
inline constexpr unsigned int kMuiAcceptedSha256Count =
    sizeof(kMuiAcceptedSha256) / sizeof(kMuiAcceptedSha256[0]);
// Expected file name at the configured candidate locations.
inline constexpr wchar_t kMuiFileName[] = L"explorer.exe.mui";

inline constexpr wchar_t kSymbolHost[] = L"msdl.microsoft.com";
inline constexpr wchar_t kSymbolPathTemplate[] =
    L"/download/symbols/explorer.exe/%08X%x/explorer.exe";
// -> /download/symbols/explorer.exe/4CE7A1442C0000/explorer.exe

// ----- import patch --------------------------------------------------------
inline constexpr const char* kPatchTargets[] = {
    "SHLWAPI.DLL", "OLE32.DLL", "EXPLORERFRAME.DLL",
};
inline constexpr const char* kWrapperName = "wrp64.dll";

// ----- robustness policy ---------------------------------------------------
inline constexpr unsigned long kConnectTimeoutMs = 10'000;
inline constexpr unsigned long kSendTimeoutMs = 15'000;
inline constexpr unsigned long kReceiveTimeoutMs = 15'000;
inline constexpr unsigned long kOverallDeadlineMs = 120'000;
inline constexpr unsigned int  kMaxAttempts = 3;
inline constexpr unsigned long kRetryBackoffMs[] = { 2'000, 5'000 };
inline constexpr unsigned int  kMaxDownloadBytes = 16u * 1024 * 1024; // hard cap
inline constexpr unsigned long kSessionGraceMs = 0; // fail fast; never block logon

// Layout inside the application folder (never touches %SystemRoot%):
//   <app>\cache\explorer-<ts>-<soi>.pris        pristine, hash-verified copy
//   <app>\state\install.json                   install record (hashes, times)
//   <app>\log\ex7setup.log                     readable action log
//   <app>\explorer.exe                          patched, localized working copy
inline constexpr wchar_t kCacheSubDir[] = L"cache";
inline constexpr wchar_t kStateSubDir[] = L"state";
inline constexpr wchar_t kLogSubDir[] = L"log";
inline constexpr wchar_t kLogFileName[] = L"ex7setup.log";
inline constexpr wchar_t kToolVersion[] = L"0.1.0";

} // namespace cfg
