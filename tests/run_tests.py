#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Runs the whole test-suite without external deps:
    python3 tests/run_tests.py
"""
import sys
import unittest
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root))

if __name__ == "__main__":
    loader = unittest.TestLoader()
    suite = loader.discover(str(root / "tests"), pattern="test_*.py")
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
