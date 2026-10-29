"""Runtime lookup on cross and native AArch64 build hosts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'panfrost_runtime', ROOT / 'scripts/linux/stage-panfrost-rootfs.py')
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class RuntimeLookup(unittest.TestCase):
    def test_explicit_runtime_precedes_compiler_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            library = base / 'libtest.so'
            library.touch()
            with patch.object(runtime.subprocess, 'check_output') as compiler:
                self.assertEqual(runtime.find_library(library.name, [base]), library)
            compiler.assert_not_called()

    def test_native_multiarch_runtime_is_found_through_cross_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / 'libstdc++.so.6'
            library.touch()
            with patch.object(runtime.subprocess, 'check_output',
                              return_value=str(library) + '\n') as compiler:
                self.assertEqual(runtime.find_library(library.name, []), library)
            compiler.assert_called_once_with(
                ['aarch64-linux-gnu-g++', '-print-file-name=libstdc++.so.6'], text=True)

    def test_unresolved_compiler_name_is_rejected(self):
        with patch.object(runtime.subprocess, 'check_output', return_value='libmissing.so\n'):
            self.assertIsNone(runtime.find_library('libmissing.so', []))


if __name__ == '__main__':
    unittest.main()
