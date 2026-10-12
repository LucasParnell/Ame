from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Console(unittest.TestCase):
    def test_watchdog_keepalive_preserves_policy_and_rejects_unknown_or_expired_hardware(self):
        with tempfile.TemporaryDirectory(prefix='ame-watchdog-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'lib/include'), str(ROOT / 'tests/sp805_watchdog_probe.cc'),
                            '-o', probe], capture_output=True, text=True, check=True)
            subprocess.run([probe], capture_output=True, text=True, check=True, timeout=10)

    def test_framebuffer_console_viewport_wrap_scroll_and_bounds(self):
        with tempfile.TemporaryDirectory(prefix='ame-framebuffer-console-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                            '-I' + str(ROOT / 'lib/include'), str(ROOT / 'tests/framebuffer_console_probe.cc'),
                            str(ROOT / 'kernel/drivers/framebuffer_console.cc'), str(ROOT / 'lib/libk/string.cc'),
                            '-o', probe], capture_output=True, text=True, check=True)
            subprocess.run([probe], capture_output=True, text=True, check=True, timeout=10)

    def test_logger_uses_board_dispatch(self):
        # Check the real AArch64 logger, including its early and panic paths.
        # A direct driver dependency bypasses both board selection and the
        # secondary diagnostic sink, even if normal console calls are correct.
        with tempfile.TemporaryDirectory(prefix='ame-log-console-') as work:
            obj = str(Path(work) / 'log.o')
            result = subprocess.run([
                'clang++', '--target=aarch64-none-elf', '-std=c++23',
                '-ffreestanding', '-mgeneral-regs-only', '-fno-exceptions', '-fno-rtti',
                '-I' + str(ROOT / 'kernel/include'),
                '-I' + str(ROOT / 'shared/include'), '-I' + str(ROOT / 'lib/include'),
                '-c', str(ROOT / 'kernel/log/log.cc'), '-o', obj],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
            result = subprocess.run([nm, '--undefined-only', '--demangle', obj],
                                    capture_output=True, text=True, check=True)
            self.assertIn('console::putchar(char)', result.stdout)
            self.assertNotIn('drivers::', result.stdout)

    def test_board_dispatch_and_secondary_sink(self):
        with tempfile.TemporaryDirectory(prefix='ame-console-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'),
                str(ROOT / 'tests/console_probe.cc'),
                str(ROOT / 'kernel/drivers/console.cc'),
                str(ROOT / 'kernel/drivers/uart/pl011.cc'),
                str(ROOT / 'kernel/drivers/uart/tegra_uart.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
