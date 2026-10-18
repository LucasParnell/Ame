"""Run real ASID switching/reuse through the production VM primitives."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_boot_cache import llvm_tool

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('qemu-system-aarch64') and all(llvm_tool(name) for name in
                     ('clang++', 'ld.lld', 'llvm-objcopy', 'llvm-objdump')),
                     'requires QEMU and cross-linking LLVM')
class Asid(unittest.TestCase):
    def test_a53_isolation_and_all_64_asids_reused_at_el1_and_el2(self):
        with tempfile.TemporaryDirectory(prefix='ame-asid-') as directory:
            elf, image = Path(directory) / 'probe', Path(directory) / 'Image'
            command = [llvm_tool('clang++'), '--target=aarch64-none-elf',
                       '--ld-path=' + llvm_tool('ld.lld'), '-std=c++23', '-O2',
                       '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-nostdlib',
                       '-mgeneral-regs-only', '-mstrict-align', '-ffunction-sections', '-fdata-sections',
                       '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                       '-I' + str(ROOT / 'lib/include'), str(ROOT / 'tests/boot_cache_entry.S'),
                       str(ROOT / 'tests/asid_qemu.cc'), '-Wl,--gc-sections',
                       '-Wl,-T,' + str(ROOT / 'scripts/linker/kernel.ld'),
                       '-Wl,--defsym,ANKO_LOAD_BASE=0x40080000', '-o', str(elf)]
            build = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            subprocess.run([llvm_tool('llvm-objcopy'), '-O', 'binary', str(elf), str(image)], check=True)
            assembly = subprocess.check_output([llvm_tool('llvm-objdump'), '-d',
                                                '--disassemble-symbols=_ZN2mm11vm_activateEm', str(elf)], text=True)
            self.assertNotIn('tlbi', assembly.lower())
            self.assertIn('ttbr0_el1', assembly.lower())
            for virtualization in ('off', 'on'):
                with self.subTest(virtualization=virtualization):
                    run = subprocess.run(['qemu-system-aarch64', '-machine',
                        'virt,gic-version=2,virtualization=' + virtualization, '-cpu', 'cortex-a53',
                        '-m', '128M', '-display', 'none', '-serial', 'none', '-monitor', 'none',
                        '-semihosting-config', 'enable=on,target=native', '-kernel', str(image)],
                        capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stderr)
