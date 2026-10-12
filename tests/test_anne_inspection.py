import gzip
import importlib.util
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('anne_inspection', ROOT / 'scripts/anne/inspect-recovery.py')
inspection = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspection)


def minimal_fdt():
    # A complete empty-root FDT, with an empty reservation map and string block.
    body = struct.pack('>I', 1) + b'\0' * 4 + struct.pack('>II', 2, 9)
    return struct.pack('>10I', 0xD00DFEED, 56 + len(body), 56, 56 + len(body),
                       40, 17, 16, 0, 0, len(body)) + b'\0' * 16 + body


class FakeAdb:
    def __init__(self, *, config=None, fdt=None):
        self.serial = None
        self.calls = []
        self.devices = b'List of devices attached\nSYNTHETIC-SERIAL-ONLY\trecovery\n'
        self.properties = {'ro.product.model': 'ANE-LX1', 'ro.product.device': 'hi6250',
                           'ro.twrp.version': '3.4.0-0', 'ro.bootmode': 'recovery'}
        self.observations = {
            'architecture': b'aarch64\n',
            'cmdline': b'console=ttyAMA0 androidboot.serialno=SYNTHETIC-SERIAL-ONLY '
                       b'imei=987654321012345 other=value\n',
            'kexec_tool': b'/sbin/kexec\n',
            'kexec_disabled': b'0\n',
        }
        self.artifacts = {
            '/proc/config.gz': gzip.compress(config or b'CONFIG_ARM64=y\nCONFIG_KEXEC=y\n'
                                             b'# CONFIG_KEXEC_FILE is not set\n', mtime=0),
            '/sys/firmware/fdt': minimal_fdt() if fdt is None else fdt,
        }
        self.timeout = None

    def run(self, args):
        args = tuple(args)
        self.calls.append(args)
        if args == self.timeout:
            raise subprocess.TimeoutExpired(args, 1)
        result = None
        if args == ('devices',):
            result = self.devices
        elif args[:2] == ('shell', 'getprop'):
            result = (self.properties.get(args[2], '') + '\n').encode()
        elif args[:2] == ('exec-out', 'cat'):
            result = self.artifacts.get(args[2])
        else:
            for name, command in inspection.OBSERVATIONS.items():
                if args == ('shell',) + command:
                    result = self.observations.get(name)
                    break
        return subprocess.CompletedProcess(args, 0 if result is not None else 1,
                                           result or b'', b'' if result is not None else b'not available\n')


class AnneInspection(unittest.TestCase):
    def test_transport_selection_rejects_absent_ambiguous_and_unusable_devices(self):
        for devices in ([], [('one', 'recovery'), ('two', 'device')],
                        [('one', 'unauthorized')], [('one', 'offline')], [('one', 'sideload')]):
            with self.subTest(devices=devices), self.assertRaises(ValueError):
                inspection.select_device(devices)
        parsed = inspection.parse_devices('List of devices attached\n\na\trecovery\nb\tdevice\n')
        self.assertEqual(inspection.select_device(parsed, 'a'), ('a', 'recovery'))
        with self.assertRaises(ValueError):
            inspection.select_device(parsed, 'absent')

    def test_identity_rejects_other_models_and_non_twrp_environments(self):
        for values in ({'ro.product.model': 'MAR-LX1', 'ro.twrp.version': '3.4'},
                       {'ro.product.model': 'ANE-LX1'},
                       {'ro.product.device': 'hi6250', 'ro.twrp.version': '3.4'}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                inspection.identify(values, 'recovery')
        self.assertEqual(inspection.identify({'ro.product.device': 'anne', 'twrp.version': '3.4'},
                                             'device')['status'], 'reported-anne-in-twrp')

    def test_collection_is_read_only_and_redacts_transport_identifiers(self):
        with tempfile.TemporaryDirectory() as work:
            adb = FakeAdb()
            output = inspection.collect(adb, Path(work) / 'capture')
            report = json.loads((output / 'report.json').read_text())
            summary = inspection.summarize(report, output)
            self.assertEqual(summary['kexec']['support'], 'configured-not-tested')
            self.assertFalse(summary['native_boot']['ready'])
            self.assertTrue(summary['fdt_captured'])
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
            self.assertEqual((output / 'observations/firmware.dtb').read_bytes(), minimal_fdt())
            allowed = {('devices',)}
            allowed.update(('shell', 'getprop', prop) for prop in inspection.PROPERTIES)
            allowed.update(('shell',) + cmd for cmd in inspection.OBSERVATIONS.values())
            allowed.update(('exec-out', 'cat', path) for path, _ in inspection.ARTIFACTS.values())
            self.assertEqual(set(adb.calls), allowed)
            for path in output.rglob('*'):
                if path.suffix in ('.json', '.md', '.txt'):
                    text = path.read_text()
                    self.assertNotIn('SYNTHETIC-SERIAL-ONLY', text)
                    self.assertNotIn('987654321012345', text)

    def test_other_device_is_rejected_before_hardware_queries_or_host_output(self):
        with tempfile.TemporaryDirectory() as work:
            adb = FakeAdb()
            adb.properties['ro.product.model'] = 'MAR-LX1'
            output = Path(work) / 'capture'
            with self.assertRaisesRegex(ValueError, 'refusing inspection'):
                inspection.collect(adb, output)
            self.assertFalse(output.exists())
            self.assertTrue(all(cmd == ('devices',) or cmd[:2] == ('shell', 'getprop')
                                for cmd in adb.calls))

    def test_missing_config_is_unknown_and_does_not_justify_a_native_handoff(self):
        with tempfile.TemporaryDirectory() as work:
            adb = FakeAdb()
            del adb.artifacts['/proc/config.gz']
            output = inspection.collect(adb, Path(work) / 'capture')
            report = json.loads((output / 'report.json').read_text())
            self.assertEqual(report['summary']['kexec']['support'], 'unknown')
            self.assertIsNone(report['summary']['config']['CONFIG_KEXEC'])
            self.assertFalse(report['summary']['native_boot']['ready'])

    def test_explicitly_disabled_kexec_and_runtime_lockout_are_reported(self):
        with tempfile.TemporaryDirectory() as work:
            adb = FakeAdb(config=b'CONFIG_ARM64=y\n# CONFIG_KEXEC is not set\n'
                                 b'# CONFIG_KEXEC_FILE is not set\n')
            adb.observations['kexec_disabled'] = b'1\n'
            output = inspection.collect(adb, Path(work) / 'capture')
            summary = json.loads((output / 'report.json').read_text())['summary']
            self.assertEqual(summary['kexec']['support'], 'disabled-in-captured-config')
            self.assertTrue(summary['kexec']['load_disabled'])
            self.assertIn('running kernel reports kexec loading disabled', summary['native_boot']['blockers'])

    def test_missing_file_kexec_symbol_is_not_treated_as_explicitly_disabled(self):
        options = inspection.config_options('# CONFIG_KEXEC is not set\n')
        self.assertEqual(options['CONFIG_KEXEC'], 'n')
        self.assertIsNone(options['CONFIG_KEXEC_FILE'])

    def test_timeout_and_malformed_binary_are_recorded_without_false_evidence(self):
        with tempfile.TemporaryDirectory() as work:
            adb = FakeAdb(fdt=b'cat: /sys/firmware/fdt: not found\n')
            adb.timeout = ('shell', 'cat', '/sys/class/graphics/fb0/stride')
            adb.artifacts['/proc/config.gz'] = b'not a gzip stream'
            output = inspection.collect(adb, Path(work) / 'capture')
            report = json.loads((output / 'report.json').read_text())
            self.assertEqual(report['observations']['framebuffer_stride']['status'], 'timeout')
            self.assertEqual(report['artifacts']['kernel_config']['status'], 'invalid')
            self.assertEqual(report['artifacts']['fdt']['status'], 'invalid')
            self.assertFalse(report['summary']['fdt_captured'])
            self.assertEqual(report['summary']['kexec']['support'], 'unknown')

    def test_saved_capture_corruption_and_path_escape_are_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            output = inspection.collect(FakeAdb(), Path(work) / 'capture')
            report = json.loads((output / 'report.json').read_text())
            path = output / report['observations']['architecture']['path']
            path.write_bytes(b'armv7l\n')
            with self.assertRaisesRegex(ValueError, 'integrity check failed'):
                inspection.summarize(report, output)
            path.write_bytes(b'aarch64\n')
            report['observations']['architecture']['path'] = '../outside.txt'
            with self.assertRaisesRegex(ValueError, 'leaves the inspection directory'):
                inspection.summarize(report, output)

    def test_existing_output_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as work:
            output = Path(work)
            marker = output / 'preserved.txt'
            marker.write_text('original')
            with self.assertRaises(FileExistsError):
                inspection.collect(FakeAdb(), output)
            self.assertEqual(marker.read_text(), 'original')

    def test_invalid_fdt_bounds_and_oversized_config_are_rejected(self):
        for offset, value in ((4, 9999), (8, 9999), (16, 41), (20, 16)):
            with self.subTest(offset=offset):
                data = bytearray(minimal_fdt())
                struct.pack_into('>I', data, offset, value)
                with self.assertRaises(ValueError):
                    inspection.validate_fdt(data)
        with self.assertRaisesRegex(ValueError, 'size limit'):
            inspection.config_text(gzip.compress(b'A' * (inspection.MAX_CONFIG + 1), mtime=0))

    def test_offline_cli_checks_saved_evidence_without_adb(self):
        with tempfile.TemporaryDirectory() as work:
            output = inspection.collect(FakeAdb(), Path(work) / 'capture')
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/anne/inspect-recovery.py'),
                                     'summarize', str(output / 'report.json')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Native boot remains pending', result.stdout)
            self.assertIn('configured-not-tested', result.stdout)

    def test_cli_missing_adb_and_duplicate_report_keys_fail_cleanly(self):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/anne/inspect-recovery.py'),
                                 'collect', '--adb', '/does/not/exist/adb'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('Traceback', result.stderr)
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'report.json'
            path.write_text('{"schema":1,"schema":1}')
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/anne/inspect-recovery.py'),
                                     'summarize', str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('duplicate report key', result.stderr)

    def test_cli_uses_the_selected_transport_and_preserves_binary_exec_out(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            fake = root / 'adb'
            log = root / 'calls.jsonl'
            fake.write_text('#!' + sys.executable + '\n' + '''
import gzip, json, sys
from pathlib import Path
args = sys.argv[1:]
with Path(__file__).with_name('calls.jsonl').open('a') as log:
    log.write(json.dumps(args) + '\\n')
if args == ['devices']:
    print('List of devices attached\\nSYNTHETIC-SERIAL-ONLY\\trecovery')
    raise SystemExit(0)
if args[:2] != ['-s', 'SYNTHETIC-SERIAL-ONLY']:
    raise SystemExit(2)
args = args[2:]
if args[:2] == ['shell', 'getprop']:
    print({'ro.product.model':'ANE-LX1', 'ro.twrp.version':'3.4.0-0'}.get(args[2], ''))
elif args == ['shell', 'uname', '-m']:
    print('aarch64')
elif args == ['exec-out', 'cat', '/proc/config.gz']:
    sys.stdout.buffer.write(gzip.compress(b'CONFIG_ARM64=y\\nCONFIG_KEXEC=y\\n', mtime=0))
else:
    raise SystemExit(1)
''')
            fake.chmod(0o755)
            output = root / 'capture'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/anne/inspect-recovery.py'),
                                     'collect', '--adb', str(fake), '--output', str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((output / 'report.json').read_text())
            self.assertEqual(report['summary']['kexec']['support'], 'configured-not-tested')
            config = gzip.decompress((output / 'observations/config.gz').read_bytes())
            self.assertIn(b'CONFIG_KEXEC=y', config)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(calls[0], ['devices'])
            self.assertTrue(all(cmd[:2] == ['-s', 'SYNTHETIC-SERIAL-ONLY'] for cmd in calls[1:]))

    def test_malformed_saved_report_types_are_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            for report in ([], {'schema': 1, 'target': 'huawei-anne', 'properties': []}):
                with self.subTest(report=report), self.assertRaises(ValueError):
                    inspection.summarize(report, root)


if __name__ == '__main__':
    unittest.main()
