from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SharedArena(unittest.TestCase):
    """Phase L (spec §12.3): dynamic shared arenas.

    The probe compiles the real attach/teardown path (kernel/vm/vm_shared.cc)
    against the real stage-2 builder and the real IOMMU core, so the overlap,
    lifetime-coupling and non-DMA-unless-registered rules are asserted against
    the code that ships, not a stand-in.
    """

    def test_attach_lifetime_and_dma_class(self):
        with tempfile.TemporaryDirectory(prefix='ame-shared-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/kernel/shared_arena_probe.cc'),
                str(ROOT/'kernel/vm/vm_shared.cc'),
                str(ROOT/'kernel/vm/stage2.cc'),
                str(ROOT/'kernel/iommu/core.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('shared arena probe ok', result.stdout)
