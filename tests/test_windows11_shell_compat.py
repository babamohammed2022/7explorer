"""Source-level safeguards for Win11 pinning and best-effort AutoPlay glue.

The Windows COM/window code cannot be executed in the cross-platform Python
suite; these assertions protect the important wiring until a Windows build and
runtime test are available.
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Windows11ShellCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        wrapper = ROOT / "explorerwrapper"
        cls.dllmain = (wrapper / "dllmain.cpp").read_text(encoding="latin-1")
        cls.pinned = (wrapper / "PinnedList.cpp").read_text(encoding="utf-8")
        cls.autoplay = (wrapper / "AutoPlay.cpp").read_text(encoding="utf-8")
        cls.shell_fixes = (wrapper / "ShellFixes.cpp").read_text(encoding="utf-8")

    def test_win11_taskbar_pin_falls_back_to_ipinnedlist3(self):
        self.assertIn("build >= 26100 && CTaskbandPin_CreateInstance", self.dllmain)
        self.assertIn("TryCreateWin11TaskbarPinList(native.PutVoid())", self.dllmain)
        self.assertIn("unknown->QueryInterface(IID_IPinnedList3, ppv)", self.dllmain)
        compat_start = self.dllmain.index("static HRESULT CreatePinnedListCompatibilityObject")
        factory_call = self.dllmain.index("hr = TryCreateWin11TaskbarPinList", compat_start)
        legacy_call = self.dllmain.index("IID_IPinnedList2, native.PutVoid()", compat_start)
        self.assertLess(factory_call, legacy_call)
        self.assertIn("const int caller = m_isTaskbarList ? PMC_CONTEXTMENU : PMC_STARTMENU", self.pinned)

    def test_pinned_list_adapter_owns_native_com_reference(self):
        self.assertIn("native.Detach(); // the adapter now owns", self.dllmain)
        self.assertIn("SafePinnedRelease(m_native", self.pinned)
        self.assertIn("riid == IID_IUnknown || riid == IID_IPinnedList2", self.pinned)

    def test_autoplay_listener_handles_volume_arrival_and_verb(self):
        self.assertIn("DBT_DEVICEARRIVAL", self.autoplay)
        self.assertIn("DBT_DEVTYP_VOLUME", self.autoplay)
        self.assertIn('info.lpVerb = L"autoplay"', self.autoplay)
        self.assertIn("ShellExecuteExW(&info)", self.autoplay)
        self.assertIn("AutoPlayDeviceNotifications", self.autoplay)
        self.assertIn("StartAutoPlayDeviceMonitor", self.shell_fixes)

    def test_autoplay_respects_system_policy_and_is_documented_best_effort(self):
        self.assertIn('ReadPolicyMask(L"NoDriveTypeAutoRun")', self.autoplay)
        self.assertIn('ReadPolicyMask(L"NoDriveAutoRun")', self.autoplay)
        self.assertIn("IsAutoPlayGloballyDisabled()", self.autoplay)
        docs = (ROOT / "docs" / "opzioni.md").read_text(encoding="utf-8")
        troubleshooting = (ROOT / "docs" / "troubleshooting.md").read_text(encoding="utf-8")
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        mod_url = "https://windhawk.net/mods/win7-classic-autoplay-restorer"
        self.assertIn("`AutoPlayDeviceNotifications`", docs)
        self.assertIn("best-effort", docs)
        self.assertIn("Windows 7 Classic AutoPlay Dialog Restorer", troubleshooting)
        self.assertIn(mod_url, docs)
        self.assertIn(mod_url, troubleshooting)
        self.assertIn(mod_url, readme)

    def test_taskbar_pinning_is_documented_as_unresolved_and_experimental(self):
        docs = (ROOT / "docs" / "opzioni.md").read_text(encoding="utf-8")
        troubleshooting = (ROOT / "docs" / "troubleshooting.md").read_text(encoding="utf-8")
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("not working on Windows 11", readme)
        self.assertIn("experimental compatibility path", readme)
        self.assertIn("still does not work on Windows 11", docs)
        self.assertIn("experimental", docs)
        self.assertIn("pinning from the Windows 7 taskbar **does not work**", troubleshooting)
        self.assertIn("do not solve the problem yet", troubleshooting)

    def test_com_and_window_calls_have_seh_guards(self):
        self.assertIn("SafePinnedCall", self.pinned)
        self.assertIn("SafeAutoPlayCall", self.autoplay)
        self.assertIn("AutoPlay WM_DEVICECHANGE", self.autoplay)
        self.assertIn("ScopedRegKey", self.autoplay)
        self.assertIn("ScopedHandle thread(CreateThread", self.autoplay)


if __name__ == "__main__":
    unittest.main()
