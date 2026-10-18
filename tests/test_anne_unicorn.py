"""Optional Unicorn analysis tests; firmware dumps are not required."""

import importlib.util
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
AVAILABLE = all(importlib.util.find_spec(name) for name in ('unicorn', 'capstone'))
if AVAILABLE:
    SPEC = importlib.util.spec_from_file_location('anne_unicorn', ROOT / 'scripts/anne/emulate-fastboot.py')
    emulation = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(emulation)
    REPLAY_SPEC = importlib.util.spec_from_file_location('anne_replay', ROOT / 'scripts/anne/replay-fastboot.py')
    replay = importlib.util.module_from_spec(REPLAY_SPEC)
    REPLAY_SPEC.loader.exec_module(replay)
    BMP_SPEC = importlib.util.spec_from_file_location(
        'anne_oeminfo_bmp', ROOT / 'scripts/anne/analyse-oeminfo-bmp.py')
    bmp_analysis = importlib.util.module_from_spec(BMP_SPEC)
    BMP_SPEC.loader.exec_module(bmp_analysis)


@unittest.skipUnless(AVAILABLE, 'optional Unicorn/Capstone dependencies are not installed')
class AnneUnicorn(unittest.TestCase):
    def make(self, code):
        return emulation.FastbootEmulator(bytes.fromhex(code), base=0x10000)

    def test_real_aarch64_execution_returns_and_records_trace(self):
        emulator = self.make('400580d2c0035fd6')  # mov x0, #42; ret
        trace = io.StringIO()
        report = emulator.run(0x10000, trace=trace)
        self.assertEqual(report['stop_reason'], 'returned')
        self.assertEqual(report['registers']['x0'], '0x2a')
        self.assertEqual(report['registers']['sp'], hex(emulation.STACK + emulation.STACK_SIZE))
        self.assertEqual([json.loads(line)['pc'] for line in trace.getvalue().splitlines()],
                         ['0x10000', '0x10004'])

    def test_until_stops_before_instruction(self):
        emulator = self.make('400580d2c0035fd6')
        report = emulator.run(0x10000, until=0x10004)
        self.assertEqual(report['stop_reason'], 'until')
        self.assertEqual(report['instructions_observed'], 1)

    def test_unmapped_access_reports_address_without_fabricating_ram(self):
        emulator = self.make('000040f9c0035fd6')  # ldr x0, [x0]; ret
        emulator.uc.reg_write(emulation.REGISTERS['x0'], 0x60000000)
        report = emulator.run(0x10000)
        self.assertEqual(report['stop_reason'], 'memory-fault')
        self.assertEqual(report['fault']['address'], '0x60000000')
        self.assertEqual(report['registers']['pc'], '0x10000')

    def test_instruction_limit_stops_infinite_loop(self):
        emulator = self.make('00000014')  # b .
        report = emulator.run(0x10000, count=10)
        self.assertEqual(report['stop_reason'], 'instruction-limit')
        self.assertEqual(report['instructions_observed'], 10)

    def test_secure_monitor_call_stops_before_execution(self):
        emulator = self.make('030000d4')  # smc #0
        report = emulator.run(0x10000)
        self.assertEqual(report['stop_reason'], 'trap')
        self.assertEqual(report['registers']['pc'], '0x10000')
        self.assertEqual(report['trap']['instruction'], 'smc #0')

    def test_overlapping_page_mapping_is_rejected(self):
        emulator = self.make('c0035fd6')
        with self.assertRaises(ValueError):
            emulator.map(0x10fff, 2)

    def test_firmware_specific_profile_rejects_unknown_image(self):
        emulator = self.make('c0035fd6')
        with self.assertRaisesRegex(ValueError, 'exact stock'):
            emulator.permission_profile('flash:kernel')

    def test_unbounded_execution_is_rejected(self):
        emulator = self.make('00000014')
        with self.assertRaises(ValueError):
            emulator.run(0x10000, count=0)
        with self.assertRaises(ValueError):
            emulator.run(0x10000, timeout_ms=0)

    def test_breakpoint_stops_before_execution_and_records_initial_registers(self):
        emulator = self.make('400580d2c0035fd6')
        report = emulator.run(0x10000, breakpoints=[0x10000])
        self.assertEqual(report['stop_reason'], 'breakpoint')
        self.assertEqual(report['instructions_observed'], 0)
        self.assertEqual(report['registers']['x0'], '0x0')
        self.assertEqual(report['initial_registers']['x30'], hex(emulation.RETURN))

    def test_trace_register_changes_describe_state_before_instruction(self):
        trace = io.StringIO()
        report = self.make('400580d2c0035fd6').run(0x10000, trace=trace, trace_registers=True)
        first, second = [json.loads(line) for line in trace.getvalue().splitlines()]
        self.assertEqual(first['register_changes']['x0'], '0x0')
        self.assertEqual(second['register_changes']['x0'], '0x2a')
        self.assertNotIn('sp', second['register_changes'])
        self.assertEqual(report['diagnostics']['returns_observed'], 1)

    def test_watch_records_old_bytes_and_write_value_without_changing_memory(self):
        emulator = self.make('010000b9c0035fd6')  # str w1, [x0]; ret
        emulator.uc.reg_write(emulation.REGISTERS['x0'], emulation.SCRATCH)
        emulator.uc.reg_write(emulation.REGISTERS['x1'], 0x12345678)
        stream = io.StringIO()
        report = emulator.run(0x10000, watches=[(emulation.SCRATCH + 1, 1)], observation_stream=stream)
        write = next(e for e in report['diagnostics']['recent_events'] if e['kind'] == 'memory-write')
        self.assertEqual(write['bytes_before'], '00000000')
        self.assertEqual(write['value'], '0x12345678')
        self.assertEqual(write['step'], 1)
        self.assertEqual(bytes(emulator.uc.mem_read(emulation.SCRATCH, 4)), bytes.fromhex('78563412'))
        self.assertEqual(len(stream.getvalue().splitlines()), report['diagnostics']['event_count'])

    def test_coverage_counts_loop_and_has_bounded_retention(self):
        report = self.make('00000014').run(0x10000, count=8, diagnostic_limit=1)
        coverage = report['diagnostics']['coverage']
        self.assertEqual(coverage['instructions'], [{'pc': '0x10000', 'hits': 8}])
        self.assertEqual(coverage['edges'], [{'from': '0x10000', 'to': '0x10000', 'hits': 7}])
        report = self.make('1f2003d51f2003d5c0035fd6').run(0x10000, diagnostic_limit=1)
        self.assertIn('instructions', report['diagnostics']['coverage']['truncated'])

    def test_fault_report_names_operation_and_failing_instruction(self):
        emulator = self.make('010000b9c0035fd6')
        emulator.uc.reg_write(emulation.REGISTERS['x0'], 0x60000000)
        report = emulator.run(0x10000)
        self.assertEqual(report['fault']['operation'], 'write-unmapped')
        self.assertEqual(report['memory_diagnostics']['invalid_writes'], 1)
        self.assertEqual(report['failure_context']['instruction'], 'str w1, [x0]')

    def test_protected_write_and_non_executable_fetch_are_distinguished(self):
        emulator = self.make('010000b9c0035fd6')
        emulator.uc.reg_write(emulation.REGISTERS['x0'], emulation.RETURN)
        report = emulator.run(0x10000)
        self.assertEqual(report['fault']['operation'], 'write-prot')
        report = emulator.run(emulation.SCRATCH)
        self.assertEqual(report['fault']['operation'], 'fetch-prot')
        self.assertEqual(report['memory_diagnostics']['invalid_fetches'], 1)

    def test_reports_remain_snapshots_when_setup_is_changed_later(self):
        emulator = self.make('c0035fd6')
        first = emulator.run(0x10000)
        emulator.map(0x60000000, emulation.PAGE)
        emulator.assumptions.append('later run')
        self.assertNotEqual(first['memory_maps'], emulator.maps)
        self.assertNotIn('later run', first['assumptions'])

    def heap_fixture(self, indirect=False):
        # A real guest call/return and a tiny allocator that writes its cursor.
        # The firmware-specific allocator is validated separately against the private dump.
        words = [0xd503201f] * 44
        words[:6] = [0xa9bf7bfd, 0x910003fd, 0xd2800100,
                     0xd63f0040 if indirect else 0x9400000d, 0xa8c17bfd, 0xd65f03c0]
        words[16:22] = [0xd2ae0001, 0xd2800402, 0xf2a60002,
                        0xf9000022, 0xd2a60000, 0xd65f03c0]
        words[32:36] = [0x39002001, 0xd65f03c0, 0x39008001, 0xd65f03c0]
        words[36:40] = [0xf9000001, 0xf9400002, 0x39400043, 0xd65f03c0]
        emulator = emulation.FastbootEmulator(struct.pack('<44I', *words), base=0x10000)
        emulator.map(emulation.HEAP_BASE, emulation.PAGE)
        emulator.uc.mem_write(emulation.SCRATCH, struct.pack('<QQ', emulation.HEAP_BASE,
                                                            emulation.HEAP_BASE + emulation.PAGE))
        emulator.heap = emulation.HeapMonitor(emulator, (emulation.HEAP_BASE, emulation.PAGE),
                                              allocator=0x10040, cursor=emulation.SCRATCH,
                                              top=emulation.SCRATCH + 8)
        if indirect:
            emulator.uc.reg_write(emulation.REGISTERS['x2'], 0x10040)
        return emulator

    def test_allocator_returns_native_pointer_size_caller_and_cursor(self):
        emulator = self.heap_fixture()
        report = emulator.run(0x10000)
        self.assertEqual(report['stop_reason'], 'returned')
        allocation, = report['heap']['allocations']
        self.assertEqual(allocation['caller'], 0x1000c)
        self.assertEqual(allocation['size'], 8)
        self.assertEqual(allocation['address'], 0x30000000)
        self.assertEqual(allocation['cursor_before'], 0x30000000)
        self.assertEqual(allocation['cursor_after'], 0x30000020)

    def test_indirect_allocator_callback_is_followed_without_stubbing(self):
        emulator = self.heap_fixture(indirect=True)
        report = emulator.run(0x10000)
        callbacks = [e for e in report['heap']['events'] if e['kind'] == 'indirect-branch']
        self.assertEqual(callbacks[0]['target'], 0x10040)
        self.assertEqual(report['heap']['allocations'][0]['caller'], 0x1000c)

    def test_heap_padding_overrun_is_distinct_from_reservation_overrun(self):
        emulator = self.heap_fixture()
        emulator.run(0x10000)
        emulator.uc.reg_write(emulation.REGISTERS['x1'], 0xaa)
        report = emulator.run(0x10080)
        violations = [e for e in report['heap']['events'] if e['kind'] == 'bounds-violation']
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]['address'], 0x30000008)
        self.assertFalse(violations[0]['crosses_reservation'])
        report = emulator.run(0x10088)
        violations = [e for e in report['heap']['events'] if e['kind'] == 'bounds-violation']
        self.assertTrue(violations[-1]['crosses_reservation'])

    def test_pointer_sized_store_and_later_target_read_are_recorded(self):
        emulator = self.heap_fixture()
        emulator.run(0x10000)
        emulator.uc.reg_write(emulation.REGISTERS['x1'], 0x30000004)
        report = emulator.run(0x10090)
        writes = [e for e in report['heap']['events'] if e['kind'] == 'pointer-sized-write'
                  and e['address'] == 0x30000000]
        self.assertEqual(writes[0]['value'], 0x30000004)
        target_reads = [e for e in report['heap']['events'] if e['kind'] == 'pointer-target-access']
        self.assertTrue(any(e['slot'] == 0x30000000 and e['address'] == 0x30000004 for e in target_reads))

    def test_stop_on_bounds_violation_and_stream_retention_limit(self):
        emulator = self.heap_fixture()
        emulator.heap.stop_on_oob = True
        emulator.heap.event_limit = 1
        stream = io.StringIO()
        emulator.run(0x10000, events=stream)
        report = emulator.run(0x10080, events=stream)
        self.assertEqual(report['stop_reason'], 'heap-bounds-violation')
        self.assertEqual(len(report['heap']['events']), 1)
        self.assertTrue(report['heap']['events_truncated'])
        self.assertEqual(len(stream.getvalue().splitlines()), report['heap']['event_count'])

    def test_until_checkpoint_captures_completed_allocation_once(self):
        emulator = self.heap_fixture()
        emulator.heap.checkpoints.add(0x10010)
        snapshots = []
        report = emulator.run(0x10000, until=0x10010, checkpoint_callback=snapshots.append)
        self.assertEqual(len(snapshots), 1)
        self.assertEqual(len(report['heap']['checkpoint_snapshots']), 1)
        self.assertEqual(snapshots[0]['allocations'][0]['status'], 'returned')
        self.assertEqual(snapshots[0]['registers']['pc'], '0x10010')
        self.assertEqual(snapshots[0]['cursor'], 0x30000020)

    def test_checkpoint_snapshot_does_not_change_with_later_pointer_writes(self):
        emulator = self.heap_fixture()
        emulator.heap.checkpoints.add(0x10010)
        report = emulator.run(0x10000)
        snapshot = report['heap']['checkpoint_snapshots'][0]
        original = json.loads(json.dumps(snapshot))
        emulator.uc.reg_write(emulation.REGISTERS['x1'], 0x30000004)
        emulator.run(0x10090)
        self.assertEqual(snapshot, original)
        self.assertEqual(snapshot['pointer_bearing_allocations'], [])
        self.assertTrue(emulator.heap.snapshot()['pointer_bearing_allocations'])


@unittest.skipUnless(AVAILABLE, 'optional Unicorn/Capstone dependencies are not installed')
class AnneOeminfoBmpAnalysis(unittest.TestCase):
    def test_synthetic_bmp_has_the_header_consumed_by_real_parser(self):
        bmp = bmp_analysis.make_bmp(2, 1, 0x20)
        self.assertEqual(bmp[:2], b'BM')
        self.assertEqual(struct.unpack_from('<I', bmp, 10)[0], 54)
        self.assertEqual(struct.unpack_from('<i', bmp, 18)[0], 2)
        self.assertEqual(struct.unpack_from('<i', bmp, 22)[0], 1)
        self.assertEqual(struct.unpack_from('<H', bmp, 28)[0], 16)
        self.assertEqual(len(bmp), 58)

    def test_allocation_comparison_separates_requested_and_reserved_capacity(self):
        allocation = {'id': 1, 'address': 0x30000000, 'size': 4,
                      'requested_end': 0x30000004, 'reserved_end': 0x30000020}
        operation = {'destination': {'start': 0x30000000, 'end': 0x30000080, 'size': 128}}
        comparison = bmp_analysis.relation(operation, allocation)
        self.assertFalse(comparison['within_requested'])
        self.assertFalse(comparison['within_reservation'])
        self.assertEqual(comparison['bytes_past_requested'], 124)
        self.assertEqual(comparison['bytes_past_reservation'], 96)


@unittest.skipUnless(AVAILABLE, 'optional Unicorn/Capstone dependencies are not installed')
class AnneReplay(unittest.TestCase):
    def fixture(self, work):
        image = work / 'firmware.bin'
        image.write_bytes(bytes.fromhex('00040091c0035fd6'))  # add x0, x0, #1; ret
        scenario = work / 'scenario.json'
        scenario.write_text(json.dumps({'schema': 1, 'name': 'synthetic',
                                        'firmware_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
                                        'expected_stop': 'returned'}))
        return image, scenario

    def test_replay_matches_without_private_firmware(self):
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            image, scenario = self.fixture(work)
            first, _ = replay.replay(image, scenario, work / 'first')
            second, summary = replay.replay(image, scenario, work / 'second', first)
            self.assertTrue(summary['execution_passed'])
            self.assertTrue(summary['replay_matched'])
            self.assertFalse(summary['normal_boot_milestone_complete'])
            self.assertIsNone(json.loads((second / 'comparison.json').read_text())['first_execution_divergence'])

    def test_replay_reports_first_register_divergence_and_setup_change(self):
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            image, scenario = self.fixture(work)
            first, _ = replay.replay(image, scenario, work / 'first')
            configuration = json.loads(scenario.read_text())
            configuration['registers'] = {'x0': '0x2a'}
            scenario.write_text(json.dumps(configuration))
            second, summary = replay.replay(image, scenario, work / 'second', first)
            difference = json.loads((second / 'comparison.json').read_text())
            self.assertFalse(summary['replay_matched'])
            self.assertTrue(difference['configuration_changed'])
            self.assertEqual(difference['first_execution_divergence']['step'], 1)
            self.assertIn('register_changes', difference['first_execution_divergence']['changed_fields'])

    def test_hash_mismatch_fails_before_creating_output(self):
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            image, scenario = self.fixture(work)
            image.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                replay.replay(image, scenario, work / 'output')
            self.assertFalse((work / 'output').exists())

    def test_scenario_rejects_unknown_fields_and_non_boolean_flags(self):
        with tempfile.TemporaryDirectory() as work:
            _, path = self.fixture(Path(work))
            configuration = json.loads(path.read_text())
            path.write_text(json.dumps({**configuration, 'entryy': '0x0'}))
            with self.assertRaisesRegex(ValueError, 'unknown scenario fields'):
                replay.load_scenario(path)
            path.write_text(json.dumps({**configuration, 'heap_watch': 'false'}))
            with self.assertRaisesRegex(ValueError, 'must be a boolean'):
                replay.load_scenario(path)

    def test_stream_comparison_detects_extra_execution_and_keeps_context(self):
        with tempfile.TemporaryDirectory() as work:
            a, b = Path(work) / 'a', Path(work) / 'b'
            a.write_text('{"step": 1}\n')
            b.write_text('{"step": 1}\n{"step": 2}\n')
            difference = replay.compare_jsonl(a, b)
            self.assertEqual(difference['line'], 2)
            self.assertEqual(difference['preceding_context'], [{'step': 1}])
            self.assertIsNone(difference['expected'])

    def test_checkpoint_order_is_required_for_success(self):
        configuration = {'name': 'wrong_order', 'expected_stop': 'until',
                         'expected_checkpoints': ['0x4ebec', '0x4ecdc', '0x4f028']}
        report = {'stop_reason': 'until', 'memory_diagnostics': {},
                  'heap': {'checkpoint_snapshots': [{'pc': pc} for pc in (0x4ecdc, 0x4ebec, 0x4f028)]}}
        summary = replay.summary_for(configuration, report)
        self.assertFalse(summary['execution_passed'])
        self.assertFalse(summary['checkpoint_order_matched'])


if __name__ == '__main__':
    unittest.main()
