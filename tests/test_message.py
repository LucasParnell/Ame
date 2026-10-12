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
                '-I' + str(ROOT / 'lib/include'),
                '-I' + str(ROOT / 'userland/lib/include'),
                str(ROOT / 'tests/message_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_nontrivial_and_oversized_messages(self):
        cases = {
            'nontrivial': ('struct T { ~T() {} };', 'inline messages must be trivially copyable'),
            'oversized': ('struct T { char data[MX_RING_INLINE_PAYLOAD + 1]; };',
                          'message exceeds the inline ring payload'),
        }
        for name, (declaration, diagnostic) in cases.items():
            with self.subTest(name=name):
                result = subprocess.run([
                    'clang++', '-std=c++23', '-fsyntax-only', '-x', 'c++',
                    '-I' + str(ROOT / 'lib/include'),
                    '-I' + str(ROOT / 'shared/include'),
                    '-I' + str(ROOT / 'userland/lib/include'), '-'],
                    input='#include "mochi/message.h"\n' + declaration +
                          '\nstatic_assert(mochi::message_size<T>() > 0);',
                    capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)


if __name__ == '__main__':
    unittest.main()
