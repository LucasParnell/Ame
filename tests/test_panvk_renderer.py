"""Guard the PanVK diagnostic against regressions that cost hardware runs.

Two failure modes are worth a host test because each one needed a flash cycle to
discover:

1. Job tracing enabled in the shipped path. Decoding every chain to unbuffered
   stderr on a 115200 baud console can consume the whole animation budget, which
   shows up on the device as a suspiciously low frame count rather than an error.
2. Silent failure returns. The native transport collapses every failure to -1,
   so a stage that returns without naming itself makes a broken frame
   indistinguishable from a busy one.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIAG = ROOT / 'linux-artifacts/panfrost-renderer/vulkan_diag.h'
RENDERER = ROOT / 'linux-artifacts/panvk-renderer/renderer.c'


class TraceBudget(unittest.TestCase):
    def test_shipped_path_does_not_enable_job_tracing(self):
        text = (ROOT / "linux-artifacts/panfrost-renderer/main.c").read_text()
        active = re.findall(r'setenv\(\s*"PANVK_DEBUG"\s*,\s*"([^"]*)"', text)
        self.assertTrue(active, 'expected PANVK_DEBUG to be set for the diagnostic')
        for value in active:
            self.assertNotIn(
                'trace', value.split(','),
                'PANVK_DEBUG includes trace on the shipped path: job decoding to '
                'unbuffered stderr starves the animation loop on a serial console')

    def test_pandecode_is_not_redirected_to_stderr(self):
        self.assertNotIn(
            'PANDECODE_DUMP_FILE', DIAG.read_text(),
            'pandecode writes per-chain decodes; redirecting it to stderr on the '
            'device competes with the frame budget and the console capture')


class SilentFailure(unittest.TestCase):
    def test_every_execute_failure_names_its_stage(self):
        text = DIAG.read_text()
        body = text.split('static int vulkan_rpc_execute', 1)
        self.assertEqual(len(body), 2, 'expected a vulkan_rpc_execute definition')
        # Every bare `return -1` in the execute path should instead report.
        bare = re.findall(r'return -1;', body[1].split('#else', 1)[0])
        self.assertEqual(
            bare, [],
            'vulkan_rpc_execute has unreported `return -1` paths; use vulkan_fail() so '
            'a failed frame names its stage instead of surfacing as a bare -1')

    def test_reports_a_stage_helper(self):
        text = DIAG.read_text()
        self.assertIn('vulkan_fail', text)
        self.assertIn('panvk renderer:', text)


class ClientContract(unittest.TestCase):
    def test_check_macro_still_reports_vkresult(self):
        text = RENDERER.read_text()
        self.assertIn('VkResult=%d', text,
                      'CHECK should keep printing the VkResult it saw')


if __name__ == '__main__':
    unittest.main()
