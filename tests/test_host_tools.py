import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HostTools(unittest.TestCase):
    def test_macos_keeps_project_python_ahead_of_brew(self):
        with tempfile.TemporaryDirectory(prefix='ame-host-tools-') as work:
            root = Path(work)
            scripts = root / 'scripts'
            scripts.mkdir()
            (scripts / 'host.sh').write_bytes((ROOT / 'scripts/host.sh').read_bytes())
            shims = root / 'shims'
            shims.mkdir()
            brew = root / 'brew'
            project_python = root / '.venv/bin/python3'
            fixtures = {
                shims / 'uname': 'echo Darwin',
                shims / 'brew': 'printf "%s\\n" "$TEST_BREW_PREFIX"',
                brew / 'bin/python3': 'echo global',
                project_python: 'echo project',
            }
            for path, body in fixtures.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#!/bin/sh\n' + body + '\n')
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(shims) + os.pathsep + os.environ['PATH'],
                       TEST_BREW_PREFIX=str(brew))
            result = subprocess.run(['bash', '-c',
                                     'source "$1"; ame_host_tools; python3',
                                     'test-host', str(scripts / 'host.sh')],
                                    env=env, capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.strip(), 'project')
