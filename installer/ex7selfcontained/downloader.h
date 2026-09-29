// downloader.h — WinInet download of the pinned explorer.exe with the
// robustness rules from the performance-info-tools-restorer mod:
//  - per-phase timeouts + one overall deadline (never blocks logon);
//  - immediate cancellation on logoff/shutdown (event signalled by the
//    console control handler installed in main.cpp);
//  - retries with pause;
//  - write to a temp file, verify SHA-256, THEN MoveFileEx into the cache —
//    a partially downloaded or tampered file never reaches the cache.
#pragma once

#include <windows.h>
#include <string>

namespace ex7 {

struct DownloadOptions {
    std::wstring appDir;        // cache lives under appDir\cache
    std::wstring tmpDir;        // empty -> same as appDir\state\tmp
    HANDLE cancelEvent = nullptr; // manual-reset; signalled => abort now
    bool allowSkipVerify = false; // NEVER set in production; test hook only
};

// Log sink callback (file logger installed in main.cpp).
typedef void (*LogFn)(const wchar_t* fmt, ...);
extern LogFn g_log;

// Result: on true, destPath is the cache file containing a byte-exact,
// hash-verified copy of the pinned explorer.exe.
bool EnsurePristineExplorer(const DownloadOptions& opt, std::wstring& destPath);

} // namespace ex7
