#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Static checks for Start orb configuration, project references, and documentation."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

class StartOrbStaticTests(unittest.TestCase):
    def test_opzioni_contains_orbfile(self):
        opzioni = (ROOT / "docs" / "opzioni.md").read_text(encoding="utf-8")
        self.assertIn("`OrbFile`", opzioni)
        self.assertIn(".bmp", opzioni)
        self.assertIn(".png", opzioni)
        self.assertIn("OrbDirectory", opzioni)

    def test_readme_contains_orb_section(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("Personalizzazione pulsante Start (orb)", readme)
        self.assertIn("OrbFile", readme)
        self.assertIn("OrbDirectory", readme)
        self.assertIn("Open-Shell", readme)

    def test_vcxproj_includes_startorb(self):
        vcxproj = (ROOT / "explorerwrapper" / "explorerwrapper.vcxproj").read_text(encoding="utf-8")
        self.assertIn('<ClInclude Include="StartOrb.h" />', vcxproj)
        self.assertIn("windowscodecs.lib", vcxproj)
        self.assertIn("ole32.lib", vcxproj)

    def test_startorb_header_consistency(self):
        startorb = (ROOT / "explorerwrapper" / "StartOrb.h").read_text(encoding="utf-8")
        self.assertIn("namespace Win7ExplorerRestorer", startorb)
        self.assertNotIn("namespace ex7 {", startorb)
        self.assertIn("LoadOrbPngWithWic", startorb)
        self.assertIn("LoadOrbBitmapFromFile", startorb)
        self.assertIn("CLSID_WICImagingFactory", startorb)
        self.assertIn("GUID_WICPixelFormat32bppPBGRA", startorb)

    def test_dllmain_calls_win7explorerrestorer_namespace(self):
        dllmain = (ROOT / "explorerwrapper" / "dllmain.cpp").read_text(encoding="latin-1")
        self.assertIn("Win7ExplorerRestorer::OrbRequest", dllmain)
        self.assertIn("Win7ExplorerRestorer::SafeInvokeCtx", dllmain)
        self.assertNotIn("ex7::SafeInvokeCtx", dllmain)

if __name__ == "__main__":
    unittest.main()
