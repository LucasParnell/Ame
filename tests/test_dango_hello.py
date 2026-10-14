"""Acceptance must distinguish guest execution from VM construction."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('dango_hello', ROOT / 'scripts/test/dango-hello.py')
hello = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hello)


class HelloEvidence(unittest.TestCase):
    def test_native_readiness_is_not_guest_execution(self):
        logs = ('boot_el=2 ram=0x40000000\nvm.el2_available=1\nlinux artifacts ready\n'
                'vm created\ninit: all services ready\n')
        checks = hello.evidence(logs)
        self.assertFalse(all(checks.values()))
        self.assertFalse(checks['agent_started'])
        self.assertFalse(checks['entry_returned'])
        complete = logs + 'vcpu.first_rc=0\ndango-agent: started\ndango: linux guest responded\n'
        self.assertTrue(all(hello.evidence(complete).values()))
        self.assertFalse(all(hello.evidence(complete + 'KERNEL PANIC').values()))
        self.assertFalse(all(hello.evidence(complete.replace('vcpu.first_rc=0', 'vcpu.first_rc=1')).values()))

    def test_el1_unavailability_cannot_be_reported_as_hello(self):
        logs = ('boot_el=1 ram=0x40000000\nvm.el2_available=0\n'
                'el2 unavailable: dango VM cannot run guest\n')
        self.assertTrue(all(hello.evidence(logs, 1).values()))
        self.assertFalse(all(hello.evidence(logs, 2).values()))
        self.assertFalse(all(hello.evidence(logs + 'dango-agent: started\n', 1).values()))

    def test_hello_policy_has_vm_authority_without_device_assignment(self):
        sys.path.insert(0, str(ROOT / 'scripts/mx'))
        import system_descriptor as policy
        manifest = policy.load_profile(ROOT / 'userland/system.json', 'dango-hello')
        self.assertEqual(policy.check_manifest(manifest), [])
        services = {service['name']: service for service in manifest['services']}
        self.assertEqual(set(services), {'nameservice', 'io.console', 'dango.manager', 'dango.auth_probe'})
        self.assertEqual(services['dango.manager']['caps'], ['vm'])

    @unittest.skipUnless(shutil.which('dtc') and shutil.which('fdtget'), 'requires device-tree tools')
    def test_guest_timer_override_is_explicit_and_bounded(self):
        with tempfile.TemporaryDirectory(prefix='ame-hello-dtb-') as directory:
            root = Path(directory)
            (root / 'initramfs.cpio').write_bytes(b'initramfs-size-fixture')
            env = dict(os.environ, DANGO_LINUX_OUT=directory, DANGO_LINUX_GUEST='minimal')
            for frequency in ('0', '1920000'):
                run = subprocess.run([str(ROOT / 'scripts/linux/build-dtb.sh')],
                    env=dict(env, DANGO_LINUX_TIMER_HZ=frequency), capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                read = subprocess.run(['fdtget', '-t', 'i', str(root / 'dango-linux.dtb'),
                    '/timer', 'clock-frequency'], capture_output=True, text=True)
                if frequency == '0':
                    self.assertNotEqual(read.returncode, 0)
                else:
                    self.assertEqual(read.returncode, 0, read.stderr)
                    self.assertEqual(read.stdout.strip(), frequency)
            for malformed in ('-1', '01920000', '4294967296', '18446744073709551616', '1;2'):
                run = subprocess.run([str(ROOT / 'scripts/linux/build-dtb.sh')],
                    env=dict(env, DANGO_LINUX_TIMER_HZ=malformed), capture_output=True, text=True, timeout=20)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn('DANGO_LINUX_TIMER_HZ', run.stderr)
