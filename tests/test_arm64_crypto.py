from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLANG = '/opt/homebrew/opt/llvm/bin/clang++'


@unittest.skipUnless(Path(CLANG).exists() and shutil.which('qemu-system-aarch64'),
                     'requires ELF-capable LLVM and QEMU')
class ArmCrypto(unittest.TestCase):
    def test_sha_crc_alignment_streaming_padding_and_kernel_vector_ownership(self):
        with tempfile.TemporaryDirectory(prefix='ame-arm-crypto-') as directory:
            work = Path(directory)
            script = work / 'probe.ld'
            script.write_text('''ENTRY(_start)
SECTIONS {
 . = 0x40080000;
 .text : { *(.text.entry) *(.text*) }
 .rodata : { *(.rodata*) }
 .data : { *(.data*) }
 .bss ALIGN(16) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; }
 . += 65536; __stack_top = .;
}''')
            binary = work / 'probe.elf'
            subprocess.run([CLANG, '--target=aarch64-none-elf', '-fuse-ld=lld',
                '-std=c++23', '-O2', '-ffreestanding', '-fno-exceptions', '-fno-rtti',
                '-fno-builtin', '-fno-stack-protector', '-mgeneral-regs-only', '-mstrict-align',
                '-nostdlib', '-I'+str(ROOT/'lib/include'),
                str(ROOT/'tests/arm64_crypto_probe.cc'), str(ROOT/'tests/arm64_crypto_entry.S'),
                str(ROOT/'lib/libk/arm64/crypto.S'), str(ROOT/'kernel/arch/arm64/boot_crypto.S'),
                str(ROOT/'lib/libk/string.cc'), '-Wl,-T,'+str(script), '-o', str(binary)], check=True)
            subprocess.run(['qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a53',
                '-m', '128M', '-nographic', '-monitor', 'none', '-serial', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(binary)],
                check=True, timeout=15, capture_output=True)
