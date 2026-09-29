// downloader.cpp — see downloader.h.
// BUILD STATUS: syntax-reviewed only (sandbox has no Windows toolchain).
// Link: wininet.lib
#include "downloader.h"
#include "config.h"
#include "winhash.h"

#include <wininet.h>

#include <vector>

#pragma comment(lib, "wininet.lib")

namespace Win7ExplorerRestorer {

LogFn g_log = nullptr;
static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[1024];
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(L"%s", buf);
}

static std::wstring CacheFileName() {
    wchar_t name[128];
    _snwprintf_s(name, _countof(name), _TRUNCATE,
                 L"explorer-%08X-%X.pris", cfg::kTimeDateStamp,
                 cfg::kSizeOfImage);
    return name;
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

static bool EnsureDir(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool Cancelled(const DownloadOptions& o) {
    return o.cancelEvent &&
           WaitForSingleObject(o.cancelEvent, 0) == WAIT_OBJECT_0;
}

// One download attempt into `tmpPath`. Deletes tmpPath on any failure.
static bool TryDownloadOnce(const std::wstring& tmpPath,
                            const DownloadOptions& opt,
                            ULONGLONG deadlineTick) {
    bool ok = false;
    HINTERNET hInet = nullptr, hConn = nullptr, hReq = nullptr;
    HANDLE hFile = INVALID_HANDLE_VALUE;

    hInet = InternetOpenW(L"Win7ExplorerRestorer/1.0",
                          INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { Log(L"InternetOpen failed: %lu", GetLastError()); goto out; }

    {
        DWORD tConn = cfg::kConnectTimeoutMs;
        DWORD tSend = cfg::kSendTimeoutMs;
        DWORD tRecv = cfg::kReceiveTimeoutMs;
        InternetSetOptionW(hInet, INTERNET_OPTION_CONNECT_TIMEOUT,
                           &tConn, sizeof(tConn));
        InternetSetOptionW(hInet, INTERNET_OPTION_SEND_TIMEOUT,
                           &tSend, sizeof(tSend));
        InternetSetOptionW(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                           &tRecv, sizeof(tRecv));
    }

    hConn = InternetConnectW(hInet, cfg::kSymbolHost,
                             INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr,
                             INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) { Log(L"InternetConnect failed: %lu", GetLastError()); goto out; }
    {
        wchar_t path[256];
        _snwprintf_s(path, _countof(path), _TRUNCATE,
                     cfg::kSymbolPathTemplate, cfg::kTimeDateStamp,
                     cfg::kSizeOfImage);
        // NOTE: no INTERNET_FLAG_IGNORE_* flags anywhere — TLS/certificate
        // errors must FAIL the download, never be swallowed.
        hReq = HttpOpenRequestW(hConn, L"GET", path, nullptr, nullptr,
                                nullptr,
                                INTERNET_FLAG_SECURE |
                                INTERNET_FLAG_RELOAD |
                                INTERNET_FLAG_NO_CACHE_WRITE |
                                INTERNET_FLAG_PRAGMA_NOCACHE, 0);
    }
    if (!hReq) { Log(L"HttpOpenRequest failed: %lu", GetLastError()); goto out; }
    if (!HttpSendRequestW(hReq, nullptr, 0, nullptr, 0)) {
        Log(L"HttpSendRequest failed: %lu", GetLastError());
        goto out;
    }
    {
        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        if (!HttpQueryInfoW(hReq,
                            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                            &statusCode, &sz, nullptr) ||
            statusCode != 200) {
            Log(L"unexpected HTTP status");
            goto out;
        }
    }
    hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log(L"cannot create temp file: %lu", GetLastError());
        goto out;
    }
    {
        BYTE buf[64 * 1024];
        DWORD total = 0;
        for (;;) {
            if (Cancelled(opt)) { Log(L"cancelled during download"); goto out; }
            if (GetTickCount64() > deadlineTick) {
                Log(L"overall deadline reached mid-download");
                goto out;
            }
            DWORD rd = 0;
            if (!InternetReadFile(hReq, buf, sizeof(buf), &rd)) {
                Log(L"InternetReadFile failed: %lu", GetLastError());
                goto out;
            }
            if (rd == 0) { ok = true; break; }  // complete
            total += rd;
            if (total > cfg::kMaxDownloadBytes) {
                Log(L"download exceeds hard cap");
                goto out;
            }
            DWORD wr = 0;
            if (!WriteFile(hFile, buf, rd, &wr, nullptr) || wr != rd) {
                Log(L"temp file write failed: %lu", GetLastError());
                goto out;
            }
        }
    }
out:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hReq) InternetCloseHandle(hReq);
    if (hConn) InternetCloseHandle(hConn);
    if (hInet) InternetCloseHandle(hInet);
    if (!ok) DeleteFileW(tmpPath.c_str());
    return ok;
}

bool EnsurePristineExplorer(const DownloadOptions& opt, std::wstring& destPath) {
    destPath.clear();
    const std::wstring cacheDir = Join(opt.appDir, cfg::kCacheSubDir);
    const std::wstring stateDir = Join(opt.appDir, cfg::kStateSubDir);
    const std::wstring tmpDir = opt.tmpDir.empty()
        ? Join(stateDir, L"tmp") : opt.tmpDir;
    if (!EnsureDir(cacheDir) || !EnsureDir(stateDir) || !EnsureDir(tmpDir)) {
        Log(L"cannot create app directories under %s", opt.appDir.c_str());
        return false;
    }
    const std::wstring cachePath = Join(cacheDir, CacheFileName());

    auto hashAccepted = [](const std::wstring& hash) -> int {
        for (unsigned int i = 0; i < cfg::kAcceptedSha256Count; ++i)
            if (Sha256HexEqualsCI(hash, cfg::kAcceptedSha256[i]))
                return (int)i;
        return -1;
    };

    // --- offline reuse ------------------------------------------------------
    {
        HANDLE h = OpenForReadShared(cachePath);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring hash = Sha256HexOfHandle(h);
            CloseHandle(h);
            int v = hashAccepted(hash);
            if (v >= 0) {
                Log(L"offline: reusing verified cache %s (variant %c)",
                    cachePath.c_str(), L'A' + v);
                destPath = cachePath;
                return true;
            }
            Log(L"cache file hash mismatch -> treating as corrupt, deleting");
            DeleteFileW(cachePath.c_str());
        }
    }

    // --- download path -------------------------------------------------------
    // NOTE: GetTempFileNameW uses only the first 3 chars of the prefix,
    // hence the abbreviated project tag.
    wchar_t tmpName[MAX_PATH];
    if (!GetTempFileNameW(tmpDir.c_str(), L"W7E", 0, tmpName)) {
        Log(L"GetTempFileName failed: %lu", GetLastError());
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + cfg::kOverallDeadlineMs;

    for (unsigned int attempt = 1; attempt <= cfg::kMaxAttempts; ++attempt) {
        if (Cancelled(opt)) { Log(L"cancelled before attempt %u", attempt); return false; }
        ULONGLONG now = GetTickCount64();
        if (now > deadline) { Log(L"deadline exceeded; giving up"); return false; }

        Log(L"download attempt %u of %u", attempt, cfg::kMaxAttempts);
        if (TryDownloadOnce(tmpName, opt, deadline)) {
            // ---- verify BEFORE it ever touches the cache ----
            HANDLE h = OpenForReadShared(tmpName);
            if (h == INVALID_HANDLE_VALUE) { DeleteFileW(tmpName); return false; }
            std::wstring hash = Sha256HexOfHandle(h);
            CloseHandle(h);
            int v = hashAccepted(hash);
            if (v < 0) {
                // Loud refusal: never accept another file silently.
                Log(L"HASH MISMATCH: got %s, no match in the pinned "
                    L"allow-list (%u entries) — file deleted",
                    hash.c_str(), cfg::kAcceptedSha256Count);
                DeleteFileW(tmpName);
                return false;
            }
            Log(L"hash verified against allow-list entry [%c] (%s)",
                L'A' + v, hash.c_str());
            if (opt.allowSkipVerify) {
                Log(L"WARNING: allowSkipVerify is a test-only hook");
            }
            DWORD attrs = GetFileAttributesW(cachePath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) DeleteFileW(cachePath.c_str());
            if (!MoveFileExW(tmpName, cachePath.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                             MOVEFILE_WRITE_THROUGH)) {
                Log(L"MoveFileEx into cache failed: %lu", GetLastError());
                DeleteFileW(tmpName);
                return false;
            }
            Log(L"downloaded and verified: %s", cachePath.c_str());
            destPath = cachePath;
            return true;
        }
        if (attempt < cfg::kMaxAttempts) {
            DWORD pause = cfg::kRetryBackoffMs[attempt - 1];
            Log(L"attempt %u failed; retrying in %lu ms", attempt, pause);
            // sleep in small slices so cancellation stays immediate
            for (DWORD slept = 0; slept < pause; slept += 100) {
                if (Cancelled(opt)) return false;
                Sleep((pause - slept) < 100u ? (pause - slept) : 100u);
            }
        }
    }
    Log(L"all %u download attempts failed", cfg::kMaxAttempts);
    return false;
}

} // namespace Win7ExplorerRestorer
