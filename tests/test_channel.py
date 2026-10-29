from pathlib import Path
import importlib.util
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Channel(unittest.TestCase):
    def test_source_overlay_preserves_the_supplied_linux_tree(self):
        spec = importlib.util.spec_from_file_location('stage', ROOT/'scripts/linux/stage-gpu-source.py')
        stage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(stage)
        with tempfile.TemporaryDirectory(prefix='ame-linux-overlay-') as work:
            source, dest = Path(work)/'linux', Path(work)/'overlay'
            (source/'drivers/misc').mkdir(parents=True)
            original = source/'drivers/misc/Makefile'
            original.write_text('obj-y += original.o\n')
            (source/'Makefile').write_text('original kernel\n')
            (source/'drivers/misc/original.c').write_text('original driver\n')
            stage.stage(source, dest, ROOT)
            stamp = (dest/'drivers/misc/Makefile').stat().st_mtime_ns
            stage.stage(source, dest, ROOT)
            self.assertEqual(original.read_text(), 'obj-y += original.o\n')
            self.assertEqual(stamp, (dest/'drivers/misc/Makefile').stat().st_mtime_ns)
            self.assertTrue((dest/'Makefile').is_symlink())
            self.assertTrue((dest/'drivers/misc/original.c').is_symlink())
            self.assertFalse((dest/'drivers/misc/Makefile').is_symlink())
            self.assertIn('obj-y += dango_channel.o', (dest/'drivers/misc/Makefile').read_text())
            (dest/'drivers/misc/Makefile').unlink()
            (dest/'drivers/misc/Makefile').symlink_to(original)
            stage.stage(source, dest, ROOT)
            self.assertEqual(original.read_text(), 'obj-y += original.o\n')
            self.assertFalse((dest/'drivers/misc/Makefile').is_symlink())

    def test_uart_pass_alone_is_not_channel_acceptance(self):
        spec = importlib.util.spec_from_file_location('channel_proof', ROOT/'scripts/test/channel.py')
        proof = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(proof)
        checks, _ = proof.evidence('dango: channel test passed\n')
        self.assertFalse(all(checks.values()))
        checks, _ = proof.evidence('channel proof: faults=1 quarantines=0 routes=1 injected=2 smmu=1')
        self.assertFalse(checks['contained_dma'])
        checks, _ = proof.evidence('channel.h2g=10\nchannel.g2h=11\n')
        self.assertFalse(checks['h2g_count'])
        self.assertFalse(checks['g2h_count'])
        # Counters are cumulative (the bootstrap GPU exchange shares the channel),
        # so the row asserts a floor, not an exact total.
        checks, _ = proof.evidence('channel.h2g=512\nchannel.g2h=514\n')
        self.assertTrue(checks['h2g_count'])
        self.assertTrue(checks['g2h_count'])

    def test_notify_arm_races_and_hvc_authority(self):
        with tempfile.TemporaryDirectory(prefix='ame-channel-') as work:
            probe = str(Path(work) / 'probe')
            # Match the scheduler's native 128-bit atomic requirement on x86.
            atomics = ['-mcx16'] if platform.machine() in ('x86_64', 'AMD64') else []
            result = subprocess.run(['clang++', *atomics, '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/channel_notify_probe.cc'), str(ROOT/'kernel/vm/vm_channel_notify.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
