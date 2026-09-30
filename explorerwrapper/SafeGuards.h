#pragma once
// 7explorer fork: RAII wrappers + SEH guards.
//
// The wrapper is built with C++ exceptions DISABLED (/EH off, DllMain entry
// point, minimal CRT), so C++ try/catch cannot be used reliably here. The
// real-world failures inside explorer (bad pointers from changed Windows
// builds, COM servers that die, missing exports) are structured exceptions,
// which __try/__except catches regardless of /EH. RAII below guarantees that
// handles/COM pointers are released on every return path.
//
// Rule for SEH: a function containing __try must not own C++ objects with
// destructors (C2712), so guarded code goes through the plain thunks below.
#include "common.h"
#include "dbgprint.h"

namespace Win7ExplorerRestorer {

// ---------------------------------------------------------------- RAII
class ScopedRegKey {
public:
	ScopedRegKey() : m_h(nullptr) {}
	~ScopedRegKey() { Reset(); }
	HKEY* Put() { Reset(); return &m_h; }
	HKEY Get() const { return m_h; }
	explicit operator bool() const { return m_h != nullptr; }
	void Reset() { if (m_h) { RegCloseKey(m_h); m_h = nullptr; } }
private:
	HKEY m_h;
	ScopedRegKey(const ScopedRegKey&) = delete;
	ScopedRegKey& operator=(const ScopedRegKey&) = delete;
};

template <class T>
class ComPtr {
public:
	ComPtr() : m_p(nullptr) {}
	~ComPtr() { Reset(); }
	T** Put() { Reset(); return &m_p; }
	void** PutVoid() { return reinterpret_cast<void**>(Put()); }
	T* Get() const { return m_p; }
	T* operator->() const { return m_p; }
	explicit operator bool() const { return m_p != nullptr; }
	// Transfer the owned reference to a caller without releasing it.
	T* Detach() { T* p = m_p; m_p = nullptr; return p; }
	void Reset() { if (m_p) { T* p = m_p; m_p = nullptr; p->Release(); } }
private:
	T* m_p;
	ComPtr(const ComPtr&) = delete;
	ComPtr& operator=(const ComPtr&) = delete;
};

class ScopedCoTaskMem {
public:
	ScopedCoTaskMem() : m_p(nullptr) {}
	~ScopedCoTaskMem() { if (m_p) CoTaskMemFree(m_p); }
	PWSTR* PutStr() { if (m_p) { CoTaskMemFree(m_p); m_p = nullptr; } return reinterpret_cast<PWSTR*>(&m_p); }
	PCWSTR Str() const { return static_cast<PCWSTR>(m_p); }
private:
	void* m_p;
	ScopedCoTaskMem(const ScopedCoTaskMem&) = delete;
	ScopedCoTaskMem& operator=(const ScopedCoTaskMem&) = delete;
};

class ScopedHandle {
public:
	explicit ScopedHandle(HANDLE h = nullptr) : m_h(h) {}
	~ScopedHandle() { if (m_h && m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); }
	HANDLE Get() const { return m_h; }
	HANDLE Release() { HANDLE h = m_h; m_h = nullptr; return h; }
	void Reset(HANDLE h) { if (m_h && m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); m_h = h; }
private:
	HANDLE m_h;
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
};

// ---------------------------------------------------------------- SEH
inline int SehFilter(const wchar_t* where, EXCEPTION_POINTERS* ep)
{
	DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
	PVOID addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	// Do not swallow fatal/stack conditions that must reach the OS.
	if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */)
		return EXCEPTION_CONTINUE_SEARCH;
	dbgprintf(L"[Win7ExplorerRestorer] SEH 0x%08X at %p caught in %s", code, addr, where ? where : L"?");
	return EXCEPTION_EXECUTE_HANDLER;
}

// Runs fn() under SEH. Returns false if a structured exception occurred.
inline bool SafeInvoke(const wchar_t* where, void (*fn)())
{
	__try { fn(); return true; }
	__except (SehFilter(where, GetExceptionInformation())) { return false; }
}

template <class Ctx>
inline bool SafeInvokeCtx(const wchar_t* where, void (*fn)(Ctx*), Ctx* ctx)
{
	__try { fn(ctx); return true; }
	__except (SehFilter(where, GetExceptionInformation())) { return false; }
}

class ScopedCoInit {
public:
	explicit ScopedCoInit(DWORD dwCoInit = COINIT_APARTMENTTHREADED)
		: m_hr(CoInitializeEx(nullptr, dwCoInit)) {}
	~ScopedCoInit() {
		if (SUCCEEDED(m_hr)) {
			CoUninitialize();
		}
	}
	bool Succeeded() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
	HRESULT Result() const { return m_hr; }
private:
	HRESULT m_hr;
	ScopedCoInit(const ScopedCoInit&) = delete;
	ScopedCoInit& operator=(const ScopedCoInit&) = delete;
};

} // namespace Win7ExplorerRestorer

