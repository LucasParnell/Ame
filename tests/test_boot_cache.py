from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def llvm_tool(name):
    # Apple Clang's linker produces Mach-O; prefer the ELF-capable LLVM tools.
    for prefix in ('/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin',
                   '/usr/local/opt/llvm/bin', '/usr/local/opt/lld/bin'):
        candidate = Path(prefix) / name
        if candidate.exists():
            return str(candidate)
    return shutil.which(name)


@unittest.skipUnless(shutil.which('clang++'), 'requires clang++')
class BootCacheTables(unittest.TestCase):
    def test_identity_permissions_boundaries_and_rejected_extents(self):
        with tempfile.TemporaryDirectory(prefix='ame-boot-cache-') as directory:
            probe = Path(directory) / 'probe'
            subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/boot_cache_tables_probe.cc'), str(ROOT / 'lib/libk/string.cc'),
                '-o', str(probe)], check=True, capture_output=True)
            subprocess.run([str(probe)], check=True, capture_output=True, timeout=10)


@unittest.skipUnless(shutil.which('qemu-system-aarch64') and llvm_tool('clang++') and
                     llvm_tool('ld.lld') and llvm_tool('llvm-objcopy'),
                     'requires QEMU and cross-linking LLVM')
class BootCacheCpu(unittest.TestCase):
    def test_real_cache_scope_restores_registers_and_publishes_data_at_el1_and_el2(self):
        with tempfile.TemporaryDirectory(prefix='ame-boot-cache-cpu-') as directory:
            elf, image = Path(directory) / 'probe', Path(directory) / 'Image'
            build = subprocess.run([
                llvm_tool('clang++'), '--target=aarch64-none-elf',
                '--ld-path=' + llvm_tool('ld.lld'), '-std=c++23', '-O2',
                '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-nostdlib',
                '-mgeneral-regs-only', '-mstrict-align', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'lib/include'),
                str(ROOT / 'tests/boot_cache_entry.S'), str(ROOT / 'tests/boot_cache_qemu.cc'),
                str(ROOT / 'kernel/mm/boot_cache.cc'), str(ROOT / 'kernel/arch/arm64/boot_cache.S'),
                str(ROOT / 'kernel/platform/probe.cc'), str(ROOT / 'kernel/init/fdt.cc'),
                str(ROOT / 'lib/libk/string.cc'),
                '-Wl,-T,' + str(ROOT / 'scripts/linker/kernel.ld'),
                '-Wl,--defsym,ANKO_LOAD_BASE=0x40080000', '-o', str(elf)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            # QEMU executes DC IVAC without enforcing the real CPU's write
            # permission check. Inspect the linked restore routine too: its
            # read-only DTB invalidation must follow the MMU-off transition.
            if llvm_tool('llvm-objdump'):
                restore = subprocess.check_output([
                    llvm_tool('llvm-objdump'), '-d', '--disassemble-symbols=boot_cache_restore',
                    str(elf)], text=True).lower()
                self.assertLess(restore.index('msr\tsctlr_el1'), restore.index('dc\tivac'))
            subprocess.run([llvm_tool('llvm-objcopy'), '-O', 'binary', str(elf), str(image)],
                           check=True, capture_output=True)
            for virtualization in ('off', 'on'):
                with self.subTest(virtualization=virtualization):
                    result = subprocess.run([
                        'qemu-system-aarch64', '-machine',
                        'virt,gic-version=2,virtualization=' + virtualization,
                        '-cpu', 'cortex-a53', '-m', '128M', '-display', 'none',
                        '-serial', 'none', '-monitor', 'none',
                        '-semihosting-config', 'enable=on,target=native', '-kernel', str(image)],
                        capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
