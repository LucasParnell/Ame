import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


profile = module('profile', 'scripts/linux/check-profile.py')
proof = module('proof', 'scripts/test/gpu-appliance.py')


class GpuArtifacts(unittest.TestCase):
    def test_profile_and_stale_artifacts_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            for name in ('Image', 'initramfs.cpio', 'dango-linux.dtb'):
                (root / name).write_bytes(b'data')
            (root / 'manifest.toml').write_text(
                'profile="gpu"\nkernel_size=4\ninitramfs_size=4\ndtb_size=4\n')
            profile.check(root, 'gpu')
            with self.assertRaises(ValueError):
                profile.check(root, 'hello')
            (root / 'Image').write_bytes(b'old')
            with self.assertRaises(ValueError):
                profile.check(root, 'gpu')

    def test_uart_pass_alone_is_not_gpu_acceptance(self):
        checks, _ = proof.evidence('dango: gpu probe passed\n')
        self.assertFalse(all(checks.values()))
        checks, _ = proof.evidence('gpu proof: faults=1 quarantines=0 routes=1 injected=8 smmu=1')
        self.assertFalse(checks['contained_dma'])
        checks, _ = proof.evidence('gpu proof: faults=0 quarantines=0 routes=1 injected=0 smmu=1')
        self.assertFalse(checks['host_interrupts'])
