from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class IommuCore(unittest.TestCase):
    def test_publication_failure_rollback_and_stream_reclamation(self):
        with tempfile.TemporaryDirectory(prefix='ame-iommu-order-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'), str(ROOT/'tests/iommu_ordering_probe.cc'),
                str(ROOT/'kernel/iommu/core.cc'), '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('iommu ordering probe ok', result.stdout)

    def test_generic_iommu_core(self):
        with tempfile.TemporaryDirectory(prefix='ame-iommu-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/iommu_probe.cc'), str(ROOT/'kernel/iommu/core.cc'),
                '-o', probe], check=True, capture_output=True, text=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('iommu core probe ok', result.stdout)
