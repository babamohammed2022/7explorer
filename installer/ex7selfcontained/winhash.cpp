// winhash.cpp — CNG SHA-256 + PE identity checks + optional WinVerifyTrust.
//
// BUILD STATUS: reviewed for syntax only inside the sandbox (no Windows
// toolchain available here). Build with MSVC (cl /std:c++17 /utf-8), link:
// bcrypt.lib wintrust.lib crypt32.lib
#include "winhash.h"

#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace ex7 {

static const wchar_t* kHex = L"0123456789abcdef";

HANDLE OpenForReadShared(const std::wstring& path) {
    std::wstring p = path;
    if (p.rfind(L"\\\\?\\", 0) != 0 && p.size() >= 240)  // no MAX_PATH limits
        p = L"\\\\?\\" + p;
    return CreateFileW(p.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

std::wstring Sha256HexOfHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring result;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0)
        return {};
    DWORD objLen = 0, dummy = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen,
                          sizeof(objLen), &dummy, 0) != 0)
        goto done_alg;
    {
        std::vector<UCHAR> obj(objLen);
        if (BCryptCreateHash(alg, &hash, obj.data(), objLen,
                             nullptr, 0, 0) != 0)
            goto done_alg;

        LARGE_INTEGER pos{};
        if (SetFilePointerEx(h, pos, &pos, FILE_BEGIN) == 0)
            goto done_hash;

        UCHAR buf[64 * 1024];
        DWORD rd = 0;
        while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) {
            if (BCryptHashData(hash, buf, rd, 0) != 0)
                goto done_hash;
        }
        {
            UCHAR digest[32];
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
                goto done_hash;
            result.reserve(64);
            for (UCHAR b : digest) {
                result += kHex[b >> 4];
                result += kHex[b & 15];
            }
        }
    done_hash:
        BCryptDestroyHash(hash);
    }
done_alg:
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

bool Sha256HexEqualsCI(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

// --- PE identity ------------------------------------------------------------
bool CheckPeIdentity(const std::wstring& path, unsigned long long minBytes,
                     unsigned int wantTimeDateStamp,
                     unsigned int wantSizeOfImage,
                     std::wstring& diag) {
    HANDLE h = OpenForReadShared(path);
    if (h == INVALID_HANDLE_VALUE) {
        diag = L"cannot open " + path;
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if ((unsigned long long)sz.QuadPart < minBytes) {
        diag = L"file too small";
        CloseHandle(h);
        return false;
    }
    std::vector<UCHAR> data((size_t)sz.QuadPart);
    DWORD rd = 0, total = 0;
    while (total < data.size() &&
            ReadFile(h, data.data() + total,
                    (DWORD)((data.size() - total) > ((size_t)1 << 24)
                                ? ((size_t)1 << 24)
                                : (data.size() - total)),
                    &rd, nullptr) && rd) {
        total += rd;
    }
    CloseHandle(h);
    if (total != data.size()) {
        diag = L"short read";
        return false;
    }
    if (data.size() < 0x40 || memcmp(data.data(), "MZ", 2) != 0) {
        diag = L"no MZ signature";
        return false;
    }
    auto u32 = [&](size_t off) -> unsigned int {
        unsigned int v;
        memcpy(&v, data.data() + off, 4);
        return v;
    };
    auto u16 = [&](size_t off) -> unsigned short {
        unsigned short v;
        memcpy(&v, data.data() + off, 2);
        return v;
    };
    unsigned int pe = u32(0x3C);
    if (pe + 24 > data.size() || memcmp(data.data() + pe, "PE\0\0", 4) != 0) {
        diag = L"no PE signature";
        return false;
    }
    if (u16(pe + 4) != 0x8664) {
        diag = L"not AMD64";
        return false;
    }
    unsigned int tds = u32(pe + 8);
    if (tds != wantTimeDateStamp) {
        diag = L"TimeDateStamp mismatch";
        return false;
    }
    unsigned int sizeOpt = u16(pe + 20);
    if (u16(pe + 24) != 0x20B) {
        diag = L"not PE32+";
        return false;
    }
    if (u32(pe + 24 + 56) != wantSizeOfImage) {
        diag = L"SizeOfImage mismatch";
        return false;
    }
    // walk the sections: offsets must stay inside the file
    unsigned int nSec = u16(pe + 6);
    size_t secOff = pe + 24 + sizeOpt;
    for (unsigned int i = 0; i < nSec; ++i, secOff += 40) {
        if (secOff + 40 > data.size()) {
            diag = L"section table truncated";
            return false;
        }
        unsigned int rawSize = u32(secOff + 16), rawOff = u32(secOff + 20);
        if (rawSize && (unsigned long long)rawOff + rawSize > data.size()) {
            diag = L"section outside file";
            return false;
        }
    }
    diag = L"ok";
    return true;
}

// --- Authenticode (secondary, non-blocking) ---------------------------------
TrustStatus CheckAuthenticode(const std::wstring& path, std::wstring& diag) {
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_SAFER_FLAG;

    LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),
                            &policy, &wd);
    diag = (r == 0) ? L"signature valid"
                    : L"signature NOT valid (or not verifiable),"
                      L" WinVerifyTrust=0x" +
                          std::to_wstring(static_cast<unsigned long>(r));
    WINTRUST_DATA wc{};
    wc.cbStruct = sizeof(wc);
    wc.dwUIChoice = WTD_UI_NONE;
    wc.dwStateAction = WTD_STATEACTION_CLOSE;
    wc.pFile = &fi;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &wc);
    return (r == 0) ? TrustStatus::Valid : TrustStatus::Invalid;
}

bool SameFileById(HANDLE h1, HANDLE h2, bool& same) {
    same = false;
    FILE_ID_INFO a{}, b{};
    if (!GetFileInformationByHandleEx(h1, FileIdInfo, &a, sizeof(a)))
        return false;
    if (!GetFileInformationByHandleEx(h2, FileIdInfo, &b, sizeof(b)))
        return false;
    same = (a.VolumeSerialNumber == b.VolumeSerialNumber) &&
           (a.FileId.Identifier == b.FileId.Identifier);
    return true;
}

} // namespace ex7
