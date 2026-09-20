import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('boot_runner', ROOT / 'scripts/test/boot.py')
boot_runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_runner)


class BootRunner(unittest.TestCase):
    def test_reused_logs_cannot_pass_a_failed_boot(self):
        with tempfile.TemporaryDirectory(prefix='ame-boot-test-') as work:
            for name in ('host.log', 'guest.log'):
                Path(work, name).write_text('all services ready\n')
            process = Mock(returncode=1)
            process.poll.return_value = 1
            with patch.object(boot_runner.subprocess, 'Popen', return_value=process), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(boot_runner.run(work, 'direct', 1, 1,
                                 markers=['all services ready'], output=work))
            self.assertIn('qemu exited 1', Path(work, 'result.json').read_text())
