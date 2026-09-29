// winhash.h — hashing + PE integrity helpers.
// SHA-256 computed through CNG (bcrypt) on an ALREADY-OPEN handle, exactly
// the pattern used in the performance-info-tools-restorer mod: the file is
// opened once with FILE_SHARE_READ | FILE_SHARE_DELETE and every check is
// done on that same handle, so a file can never be swapped between the
// hash check and its use.
#pragma once

#include <windows.h>
#include <string>

namespace Win7ExplorerRestorer {

// Opens `path` read-only with share-read+share-delete and long-path support.
// Returns INVALID_HANDLE_VALUE on failure.
HANDLE OpenForReadShared(const std::wstring& path);

// SHA-256 over the full content of an open handle (reads through it,
// restoring the file pointer at the end). Empty string on failure.
std::wstring Sha256HexOfHandle(HANDLE h);

bool Sha256HexEqualsCI(const std::wstring& a, const std::wstring& b);

// Structural PE validation of the bytes of `path` (already hashed):
//  - MZ/PE magic, PE32+ optional header, machine AMD64, size >= minBytes;
//  - when exactBytes != 0, EXACT file size must match;
//  - TimeDateStamp / SizeOfImage equal to the pinned identity constants;
//  - walks sections to make sure the image maps coherently.
// Returns true and fills `diag` on success; false with a message otherwise.
bool CheckPeIdentity(const std::wstring& path, unsigned long long minBytes,
                     unsigned long long exactBytes,
                     unsigned int timeDateStamp, unsigned int sizeOfImage,
                     std::wstring& diag);

// Optional secondary control: Authenticode state of the PRISTINE file
// (before we patch it). Does not block by itself.
enum class TrustStatus { Valid, Invalid, NotChecked, ApiUnavailable };
TrustStatus CheckAuthenticode(const std::wstring& path, std::wstring& diag);

// Compares two open files by (volume serial, file index): detects whether
// two paths/handles name the SAME file (used after loading the private
// copy to prove it maps the bytes we verified on disk).
bool SameFileById(HANDLE h1, HANDLE h2, bool& same);

} // namespace Win7ExplorerRestorer
