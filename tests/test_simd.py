"""Exercise FP/NEON isolation and host timer preemption through the real EL2 monitor."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
try:
    from .test_boot_cache import llvm_tool
except ImportError:
    from test_boot_cache import llvm_tool

ROOT = Path(__file__).resolve().parents[1]


class PhysicalTimer(unittest.TestCase):
    def test_guest_register_encoding_masks_and_signed_deadlines(self):
        with tempfile.TemporaryDirectory(prefix='ame-physical-timer-') as directory:
            probe = str(Path(directory) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'), str(ROOT/'tests/arm64_timer_probe.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(shutil.which('qemu-system-aarch64') and llvm_tool('clang++') and
                     llvm_tool('ld.lld'), 'requires QEMU and cross-linking LLVM')
class SimdGuest(unittest.TestCase):
    def test_host_and_two_guests_keep_all_vectors_and_control_state_isolated(self):
        with tempfile.TemporaryDirectory(prefix='ame-simd-el2-') as directory:
            work = Path(directory)
            sources = ('tests/simd_el2_entry.S', 'tests/simd_el2_qemu.cc',
                       'tests/kernel/simd_regs.S', 'kernel/arch/arm64/el2.S',
                       'kernel/arch/arm64/fpsimd.S', 'lib/libk/string.cc')
            objects = []
            for index, source in enumerate(sources):
                obj = work / f'{index}.o'
                command = [llvm_tool('clang++'), '--target=aarch64-none-elf', '-O2',
                           '-ffreestanding', '-fno-exceptions', '-fno-rtti',
                           '-I' + str(ROOT / 'kernel/include'),
                           '-I' + str(ROOT / 'shared/include'), '-I' + str(ROOT / 'lib/include')]
                if source.endswith('.cc'):
                    command += ['-std=c++23', '-mgeneral-regs-only', '-mstrict-align']
                command += ['-c', str(ROOT / source), '-o', str(obj)]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                objects.append(str(obj))
            elf, image = work / 'probe', work / 'Image'
            result = subprocess.run([
                llvm_tool('ld.lld'), '-T', str(ROOT / 'scripts/linker/kernel.ld'),
                '--defsym=ANKO_LOAD_BASE=0x40080000', *objects, '-o', str(elf)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([llvm_tool('llvm-objcopy'), '-O', 'binary', str(elf), str(image)], check=True)
            result = subprocess.run([
                'qemu-system-aarch64', '-machine', 'virt,virtualization=on,gic-version=2',
                '-cpu', 'cortex-a53', '-m', '128M', '-display', 'none', '-serial', 'none',
                '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
                '-kernel', str(image)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
