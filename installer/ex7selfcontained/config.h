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
// CONFIRMED 2026-09-28 by the user on Windows 10 21H2 LTSC (build 19044):
// real download from the URL below -> 2.872.320 bytes; certutil SHA-256
// matches kExpectedSha256; TimeDateStamp/SizeOfImage re-checked on the file.
// Si tratta di valori binari-specifici: NON modificarli senza riverificare.
inline constexpr unsigned int  kTimeDateStamp = 0x4CE7A144;
inline constexpr unsigned int  kSizeOfImage = 0x2C0000;
inline constexpr unsigned long long kExpectedFileBytes = 2872320ULL;
inline constexpr wchar_t kExpectedSha256[] =
    L"5769e5b25c7bfbc20dbfdca2f17b751f6d968e03412705de4a16c99b2626e21b";

// Host/path are pinned: HTTPS only, fixed host, path derived ONLY from the
// two identity constants above (no user influence, no URL parsing).
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
