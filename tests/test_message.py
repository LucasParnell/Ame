from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Message(unittest.TestCase):
    def test_typed_message_codec(self):
        # mochi/message.h is the single POD<->ring-slot codec; prove the round
        # trip, the opcode/length rejection paths and the null guards under
        # ASan/UBSan.
        with tempfile.TemporaryDirectory(prefix='ame-message-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'userland/lib/include'),
                str(ROOT / 'tests/message_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
