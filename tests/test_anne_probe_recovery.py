"""Recover a real translation fault through production AArch64 vectors."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('qemu-system-aarch64') and shutil.which('clang') and shutil.which('ld.lld'),
                     'requires AArch64 QEMU and LLVM')
class ProbeRecovery(unittest.TestCase):
    def test_early_and_runtime_vectors_preserve_live_registers(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            objects = []
            for index, source in enumerate(['tests/anne_probe_recovery.S',
                                           'boards/huawei-anne/boot_diagnostics.S',
                                           'kernel/arch/arm64/vectors.S']):
                obj = work / f'{index}.o'
                subprocess.run(['clang', '--target=aarch64-none-elf', '-DANKO_BOARD_HUAWEI_ANNE',
                                '-DANKO_ANNE_DIAG_TEST', '-I'+str(ROOT / 'shared/include'),
                                '-c', str(ROOT / source), '-o', str(obj)], check=True, capture_output=True)
                objects.append(str(obj))
            linker = work / 'test.ld'
            linker.write_text('ENTRY(_start)\nSECTIONS { . = 0x40080000; '
                              '.text : { *(.text.start) *(.text*) } '
                              '.data : { *(.data*) } .bss : { *(.bss*) } }\n')
            elf = work / 'probe.elf'
            subprocess.run(['ld.lld', '-T', str(linker), *objects, '-o', str(elf)], check=True, capture_output=True)
            run = subprocess.run(['qemu-system-aarch64', '-machine', 'virt,virtualization=off',
                                  '-cpu', 'cortex-a53', '-m', '128M', '-nographic', '-monitor', 'none',
                                  '-serial', 'none', '-semihosting-config', 'enable=on,target=native',
                                  '-kernel', str(elf)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
