import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('anne_fb_inspection', ROOT / 'scripts/anne/inspect-framebuffer.py')
inspection = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspection)
LOG_SPEC = importlib.util.spec_from_file_location('anne_fb_log', ROOT / 'scripts/anne/collect-native-log.py')
logs = importlib.util.module_from_spec(LOG_SPEC)
LOG_SPEC.loader.exec_module(logs)
DFX_SPEC = importlib.util.spec_from_file_location('anne_fb_dfx', ROOT / 'scripts/anne/extract-dfx-native-log.py')
dfx = importlib.util.module_from_spec(DFX_SPEC)
DFX_SPEC.loader.exec_module(dfx)


class FramebufferInspection(unittest.TestCase):
    def test_watchdog_trace_preserves_inherited_policy_and_feed_count(self):
        trace = ('AMEANKO1 el=1\n'
                 'WATCHDOG phase=adopt status=1 id=0x141805 control=0x3 load=0x123456 value=0x123450 ris=0x0 feeds=1\n'
                 'WATCHDOG phase=observed status=1 id=0x141805 control=0x3 load=0x123456 value=0x123000 ris=0x0 feeds=72\n')
        samples = logs.decode(trace)['watchdog']
        self.assertEqual([s['phase'] for s in samples], ['adopt', 'observed'])
        self.assertEqual([s['feeds'] for s in samples], [1, 72])
        self.assertEqual(samples[-1]['id'], 0x141805)
        self.assertEqual(samples[-1]['status'], 1)
        self.assertEqual(samples[-1]['raw_status'], 0)
        for sample in samples:
            self.assertEqual(sample['control'], 3)
            self.assertEqual(sample['load'], 0x123456)

    def test_vendor_fiq_source_word_is_classified_without_treating_pc_as_cause(self):
        trace = ('AMEANKO1 el=1\nFIQ_SOURCE phase=entry word=0x0000000000000000 tick=0x1\n'
                 'FIQ_SOURCE phase=mmu.done word=0x5a5a5a5a5a5a5a4c tick=0x2\n'
                 'FIQ_SOURCE phase=fault word=0x5a5a5a5a5a5a5aa3 tick=0x3\n')
        sources = logs.decode(trace)['firmware_fiq']
        self.assertEqual([s['kind'] for s in sources], ['unknown', 'watchdog', 'ddr.security'])
        self.assertEqual(sources[-1]['reason_code'], 163)
        self.assertEqual(sources[-1]['phase'], 'fault')
        self.assertEqual(sources[-1]['tick'], 3)

    def test_fiq_trace_does_not_interpret_stale_svc_syndrome(self):
        fault = '!FAULT last=35 esr=0x56000000 pc=0x483a5c far=0x0 sp=0x533c80 cpu=0\n'
        for vector_line in ('VECTOR 6\n', 'VECTOR 6 saved_pstate=0x0000000000000085\n'):
            result = logs.decode('AMEANKO1 el=1\n' + vector_line + fault)['faults'][0]
            self.assertEqual(result['vector'], 6)
            self.assertEqual(result['kind'], 'fiq')
            self.assertFalse(result['syndrome_valid'])
            self.assertIsNone(result['exception_class'])
            self.assertIsNone(result['iss'])
        result = logs.decode('AMEANKO1 el=1\nVECTOR 4 saved_pstate=0x3c5\n' + fault)['faults'][0]
        self.assertEqual(result['kind'], 'sync')
        self.assertTrue(result['syndrome_valid'])
        self.assertEqual(result['exception_class'], 0x15)
        self.assertEqual(result['saved_pstate'], '0x3c5')

    def test_dfx_fallback_identifies_this_capture_and_excludes_older_images(self):
        def slot(tag):
            text = f'AMEANKO1 el=1\nRUN tag={tag}\nDISPLAY console.ready channel=0x6\n'.encode()
            return struct.pack('<3I', 0x43474244, len(text), len(text)) + text
        stale, current = slot('previous-image'), slot('anne-console-v1')
        data = stale + b'gap' + current + b'\0' + current
        matches = logs.dfx_records(data, {'run_tag': 'anne-console-v1'})
        self.assertEqual([offset for offset, raw in matches], [len(stale) + 3, len(stale) + 4 + len(current)])
        self.assertTrue(all(b'RUN tag=anne-console-v1\n' in raw for offset, raw in matches))
        self.assertFalse(logs.matches_run(stale.decode(errors='replace'), {'run_tag': 'anne-console-v1'}))
        with self.assertRaises(ValueError): logs.dfx_records(data, {})

    def test_dfx_extraction_uses_header_length_and_rejects_stale_data(self):
        trace = b'AMEANKO1 el=1\nDISPLAY begin phase=entry\nRESET nmi=0x4\n'
        for prefix in (b'', b'====0.0-D\n'):
            body = prefix + trace
            data = b'padding!' + struct.pack('<3I', 0x43474244, len(body), len(body)) + body + b'stale garbage AMEANKO1 old\xff'
            self.assertEqual(dfx.extract_record(data, 8), trace)
            with self.assertRaises(ValueError): dfx.extract_record(data, -1)
            with self.assertRaises(ValueError): dfx.extract_record(data, len(data))
            with self.assertRaises(ValueError): dfx.extract_record(data[:20 + len(body) - 1], 8)
            for offset in (8, 12, 16):
                broken = bytearray(data); broken[offset] ^= 0xff
                with self.assertRaises(ValueError): dfx.extract_record(broken, 8)

    def test_retained_display_summary_keeps_rejection_and_paint_evidence(self):
        trace = ('AMEANKO1 el=1\nDISPLAY begin phase=entry\n'
                 'DISPLAY candidate channel=0x0000000000000001 reject=rdma.disabled\n'
                 'DISPLAY begin phase=mmu.done\n'
                 'DISPLAY candidate channel=0x0000000000000001 reject=rdma.iova\n'
                 'DISPLAY paint=skip no_verified_scanout\nRESET nmi=0x4\n')
        result = logs.decode(trace)['display']
        self.assertEqual(result['phases'], ['entry', 'mmu.done'])
        self.assertEqual(result['candidates'], [{'channel': 1, 'rejection': 'rdma.disabled'},
                                                 {'channel': 1, 'rejection': 'rdma.iova'}])
        self.assertTrue(result['skipped']); self.assertFalse(result['painted'])
        self.assertTrue(logs.decode(trace + 'DISPLAY paint.done label=ANKO hold_seconds=5\n')['display']['painted'])

    def test_decode_actual_aarch64_layout(self):
        data = bytearray(272)
        struct.pack_into('<4sI3q', data, 0, b'FBQ1', 1, 3, 0, 0)
        data[32:39] = b'hisifb0'
        struct.pack_into('<Q4I3H2xIQ2I3H', data, 48,
                         0x40000, 29552640, 0, 0, 2, 1, 1, 0, 4320, 0, 0, 0, 0, 0, 0)
        struct.pack_into('<8I', data, 112, 1080, 2280, 1080, 4560, 0, 2280, 32, 0)
        struct.pack_into('<12I', data, 144, 16, 8, 0, 8, 8, 0, 0, 8, 0, 24, 8, 0)
        struct.pack_into('<I', data, 212, 180000000)
        result = inspection.decode(data)
        self.assertEqual(result['fixed']['smem_start'], '0x40000')
        self.assertEqual(result['fixed']['line_length'], 4320)
        self.assertEqual(result['fixed']['smem_len'], 29552640)
        self.assertEqual(result['variable']['yoffset'], 2280)
        self.assertEqual(result['variable']['red'], {'offset': 16, 'length': 8, 'msb_right': 0})
        self.assertEqual(result['variable']['pixclock_raw'], 180000000)
        for offset in (0, 4, 16, 24):
            damaged = bytearray(data); damaged[offset] ^= 0xff
            with self.assertRaises(ValueError): inspection.decode(damaged)
        damaged = bytearray(data); struct.pack_into('<q', damaged, 8, -13)
        with self.assertRaises(ValueError): inspection.decode(damaged)
        with self.assertRaises(ValueError): inspection.decode(data[:-1])

    def test_native_path_rejects_unsafe_addresses_and_routing(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'display')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'boards/huawei-anne'),
                            str(ROOT / 'tests/anne_native_display_probe.cc'), '-o', binary],
                           check=True, capture_output=True)
            subprocess.run([binary], check=True, capture_output=True, timeout=10)


if __name__ == '__main__': unittest.main()
