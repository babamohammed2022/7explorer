#include "PinnedList.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "SafeGuards.h"

namespace {

// The COM implementations supplied by Windows are in-process and version-
// dependent. A bad vtable/interface after a Windows update should fail this
// operation, not take down the private Explorer process.
template <class Interface, class Method, class... Args>
static HRESULT SafePinnedCall(const wchar_t* where, Interface* object,
	Method method, Args... args)
{
	if (!object) return E_NOINTERFACE;
	__try {
		return (object->*method)(args...);
	}
	__except (Win7ExplorerRestorer::SehFilter(where, GetExceptionInformation())) {
		return E_UNEXPECTED;
	}
}

static ULONG SafePinnedRelease(IUnknown* object, const wchar_t* where)
{
	if (!object) return 0;
	__try {
		return object->Release();
	}
	__except (Win7ExplorerRestorer::SehFilter(where, GetExceptionInformation())) {
		return 0;
	}
}

class ScopedPidl {
public:
	ScopedPidl() : m_pidl(nullptr) {}
	~ScopedPidl() { if (m_pidl) CoTaskMemFree(m_pidl); }
	PIDLIST_ABSOLUTE* Put() {
		if (m_pidl) CoTaskMemFree(m_pidl);
		m_pidl = nullptr;
		return &m_pidl;
	}
	PIDLIST_ABSOLUTE Get() const { return m_pidl; }
private:
	PIDLIST_ABSOLUTE m_pidl;
	ScopedPidl(const ScopedPidl&) = delete;
	ScopedPidl& operator=(const ScopedPidl&) = delete;
};

static BOOL IsParentPidlSafe(PCIDLIST_ABSOLUTE parent, PCIDLIST_ABSOLUTE child)
{
	if (!parent || !child) return FALSE;
	__try {
		return ILIsParent(parent, child, TRUE);
	}
	__except (Win7ExplorerRestorer::SehFilter(L"PinnedList::ILIsParent", GetExceptionInformation())) {
		return FALSE;
	}
}

} // namespace

CPinnedListWrapper::CPinnedListWrapper(IUnknown* native, int build,
	bool isTaskbarList, bool isLegacyInterface)
	: m_native(native),
	  m_pinnedList2(nullptr),
	  m_flexList(nullptr),
	  m_pinnedList3(nullptr),
	  m_pinnedList25(nullptr),
	  m_cRef(1),
	  m_isTaskbarList(isTaskbarList)
{
	// The reference returned by CoCreateInstance/QueryInterface is owned by
	// this adapter and is released exactly once in the destructor. The typed
	// pointers below are non-owning aliases to that same COM identity.
	if (!native) return;

	if (isLegacyInterface) {
		m_pinnedList2 = reinterpret_cast<IPinnedList2*>(native);
		dbgprintf(L"PinnedList: wrapping legacy IPinnedList2 (taskbar=%d)", isTaskbarList ? 1 : 0);
	}
	else if (build >= 17763) {
		m_pinnedList3 = reinterpret_cast<IPinnedList3*>(native);
		dbgprintf(L"PinnedList: wrapping IPinnedList3 (build=%d, taskbar=%d)", build, isTaskbarList ? 1 : 0);
	}
	else if (build >= 14393) {
		m_flexList = reinterpret_cast<IFlexibleTaskbarPinnedList*>(native);
		dbgprintf(L"PinnedList: wrapping IFlexibleTaskbarPinnedList (build=%d, taskbar=%d)", build, isTaskbarList ? 1 : 0);
	}
	else {
		m_pinnedList25 = reinterpret_cast<IPinnedList25*>(native);
		dbgprintf(L"PinnedList: wrapping IPinnedList25 (build=%d, taskbar=%d)", build, isTaskbarList ? 1 : 0);
	}
}

CPinnedListWrapper::~CPinnedListWrapper()
{
	SafePinnedRelease(m_native, L"PinnedList::~CPinnedListWrapper");
	m_native = nullptr;
}

HRESULT __stdcall CPinnedListWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject) return E_POINTER;
	*ppvObject = nullptr;
	if (!m_native) return E_NOINTERFACE;

	// Preserve this wrapper's COM identity. Forwarding IUnknown directly to
	// the native object would make later QueryInterface calls bypass the
	// pinning policy and the Win11 compatibility layer.
	if (riid == IID_IUnknown || riid == IID_IPinnedList2) {
		*ppvObject = static_cast<IPinnedList2*>(this);
		AddRef();
		return S_OK;
	}
	return SafePinnedCall(L"PinnedList::QueryInterface", m_native,
		&IUnknown::QueryInterface, riid, ppvObject);
}

ULONG __stdcall CPinnedListWrapper::AddRef(void)
{
	return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
}

ULONG __stdcall CPinnedListWrapper::Release(void)
{
	LONG count = InterlockedDecrement(&m_cRef);
	if (count == 0) {
		delete this;
		return 0;
	}
	return count > 0 ? static_cast<ULONG>(count) : 0;
}

HRESULT __stdcall CPinnedListWrapper::EnumObjects(IEnumFullIDList** p1)
{
	if (p1) *p1 = nullptr;
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::EnumObjects", m_pinnedList2, &IPinnedList2::EnumObjects, p1);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::EnumObjects", m_pinnedList25, &IPinnedList25::EnumObjects, p1);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::EnumObjects", m_flexList, &IFlexibleTaskbarPinnedList::EnumObjects, p1);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::EnumObjects", m_pinnedList3, &IPinnedList3::EnumObjects, p1);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::Modify(PCIDLIST_ABSOLUTE p1, PCIDLIST_ABSOLUTE p2)
{
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::Modify", m_pinnedList2, &IPinnedList2::Modify, p1, p2);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::Modify", m_pinnedList25, &IPinnedList25::Modify, p1, p2);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::Modify", m_flexList, &IFlexibleTaskbarPinnedList::Modify, p1, p2);
	if (m_pinnedList3) {
		// Win7's Start menu and taskbar use separate CLSIDs. Keep the original
		// Start-menu caller for Start pins, but report an explicit context-menu
		// action for a taskbar pin (rather than mislabelling it PMC_STARTMENU).
		const int caller = m_isTaskbarList ? PMC_CONTEXTMENU : PMC_STARTMENU;
		return SafePinnedCall(L"PinnedList::Modify", m_pinnedList3, &IPinnedList3::Modify, p1, p2, caller);
	}
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::GetChangeCount(ULONG* p1)
{
	if (!p1) return E_POINTER;
	*p1 = 0;
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::GetChangeCount", m_pinnedList2, &IPinnedList2::GetChangeCount, p1);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::GetChangeCount", m_pinnedList25, &IPinnedList25::GetChangeCount, p1);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::GetChangeCount", m_flexList, &IFlexibleTaskbarPinnedList::GetChangeCount, p1);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::GetChangeCount", m_pinnedList3, &IPinnedList3::GetChangeCount, p1);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::GetPinnableInfo(IDataObject* p1, int p2,
	IShellItem2** p3, IShellItem** p4, PWSTR* p5, INT* p6)
{
	if (p3) *p3 = nullptr;
	if (p4) *p4 = nullptr;
	if (p5) *p5 = nullptr;
	if (p6) *p6 = 0;
	if (m_isTaskbarList && !s_UseTaskbarPinning)
		return E_NOINTERFACE;
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::GetPinnableInfo", m_pinnedList2, &IPinnedList2::GetPinnableInfo, p1, p2, p3, p4, p5, p6);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::GetPinnableInfo", m_pinnedList25, &IPinnedList25::GetPinnableInfo, p1, p2, p3, p4, p5, p6);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::GetPinnableInfo", m_flexList, &IFlexibleTaskbarPinnedList::GetPinnableInfo, p1, p2, p3, p4, p5, p6);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::GetPinnableInfo", m_pinnedList3, &IPinnedList3::GetPinnableInfo, p1, p2, p3, p4, p5, p6);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::IsPinnable(IDataObject* p1, int p2)
{
	if (m_isTaskbarList && !s_UseTaskbarPinning)
		return E_NOINTERFACE;
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::IsPinnable", m_pinnedList2, &IPinnedList2::IsPinnable, p1, p2);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::IsPinnable", m_pinnedList25, &IPinnedList25::IsPinnable, p1, p2);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::IsPinnable", m_flexList, &IFlexibleTaskbarPinnedList::IsPinnable, p1, p2);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::IsPinnable", m_pinnedList3, &IPinnedList3::IsPinnable, p1, p2);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::Resolve(HWND p1, ULONG p2,
	PCIDLIST_ABSOLUTE p3, PIDLIST_ABSOLUTE* p4)
{
	if (p4) *p4 = nullptr;
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::Resolve", m_pinnedList2, &IPinnedList2::Resolve, p1, p2, p3, p4);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::Resolve", m_pinnedList25, &IPinnedList25::Resolve, p1, p2, p3, p4);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::Resolve", m_flexList, &IFlexibleTaskbarPinnedList::Resolve, p1, p2, p3, p4);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::Resolve", m_pinnedList3, &IPinnedList3::Resolve, p1, p2, p3, p4);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::IsPinned(PCIDLIST_ABSOLUTE p1)
{
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::IsPinned", m_pinnedList2, &IPinnedList2::IsPinned, p1);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::IsPinned", m_pinnedList25, &IPinnedList25::IsPinned, p1);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::IsPinned", m_flexList, &IFlexibleTaskbarPinnedList::IsPinned, p1);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::IsPinned", m_pinnedList3, &IPinnedList3::IsPinned, p1);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::GetPinnedItem(PCIDLIST_ABSOLUTE p1, PIDLIST_ABSOLUTE* p2)
{
	if (p2) *p2 = nullptr;
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::GetPinnedItem", m_pinnedList2, &IPinnedList2::GetPinnedItem, p1, p2);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::GetPinnedItem", m_pinnedList25, &IPinnedList25::GetPinnedItem, p1, p2);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::GetPinnedItem", m_flexList, &IFlexibleTaskbarPinnedList::GetPinnedItem, p1, p2);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::GetPinnedItem", m_pinnedList3, &IPinnedList3::GetPinnedItem, p1, p2);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::GetAppIDForPinnedItem(PCIDLIST_ABSOLUTE p1, PWSTR* p2)
{
	if (p2) *p2 = nullptr;
	if (m_isTaskbarList && !s_UseTaskbarPinning)
		return E_NOINTERFACE;

	// Only hide immersive items when the taskbar option asks us to. Start-menu
	// pinning is independent of StoreAppsOnTaskbar.
	if (m_isTaskbarList && !s_ShowStoreAppsOnTaskbar && p1) {
		ScopedPidl appsFolder;
		if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY,
			nullptr, appsFolder.Put())) && appsFolder.Get() && IsParentPidlSafe(appsFolder.Get(), p1))
			return E_NOINTERFACE;
	}

	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::GetAppIDForPinnedItem", m_pinnedList2, &IPinnedList2::GetAppIDForPinnedItem, p1, p2);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::GetAppIDForPinnedItem", m_pinnedList25, &IPinnedList25::GetAppIDForPinnedItem, p1, p2);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::GetAppIDForPinnedItem", m_flexList, &IFlexibleTaskbarPinnedList::GetAppIDForPinnedItem, p1, p2);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::GetAppIDForPinnedItem", m_pinnedList3, &IPinnedList3::GetAppIDForPinnedItem, p1, p2);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::ItemChangeNotify(PCIDLIST_ABSOLUTE p1,
	PCIDLIST_ABSOLUTE p2)
{
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::ItemChangeNotify", m_pinnedList2, &IPinnedList2::ItemChangeNotify, p1, p2);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::ItemChangeNotify", m_pinnedList25, &IPinnedList25::ItemChangeNotify, p1, p2);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::ItemChangeNotify", m_flexList, &IFlexibleTaskbarPinnedList::ItemChangeNotify, p1, p2);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::ItemChangeNotify", m_pinnedList3, &IPinnedList3::ItemChangeNotify, p1, p2);
	return E_NOINTERFACE;
}

HRESULT __stdcall CPinnedListWrapper::UpdateForRemovedItemsAsNecessary(VOID)
{
	if (m_pinnedList2)
		return SafePinnedCall(L"PinnedList::UpdateForRemovedItemsAsNecessary", m_pinnedList2, &IPinnedList2::UpdateForRemovedItemsAsNecessary);
	if (m_pinnedList25)
		return SafePinnedCall(L"PinnedList::UpdateForRemovedItemsAsNecessary", m_pinnedList25, &IPinnedList25::UpdateForRemovedItemsAsNecessary);
	if (m_flexList)
		return SafePinnedCall(L"PinnedList::UpdateForRemovedItemsAsNecessary", m_flexList, &IFlexibleTaskbarPinnedList::UpdateForRemovedItemsAsNecessary);
	if (m_pinnedList3)
		return SafePinnedCall(L"PinnedList::UpdateForRemovedItemsAsNecessary", m_pinnedList3, &IPinnedList3::UpdateForRemovedItemsAsNecessary);
	return E_NOINTERFACE;
}
