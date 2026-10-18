"""Benchmark integrity: reject incomplete runs and incompatible comparisons."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.ipc import results


def records(scope='both', cost=100):
    header = dict(record='header', version=1, contract_sha256='a' * 64,
                  transport='test', diagnostic_scope='client_process', placement_scope=scope,
                  iterations=16, warmup=1, samples=16, rounds=3, window=3, payload_mask=1,
                  include_spin=False, client_cpu=0, client_process=10, server_process=11)
    rows = [header]
    for offset in range(0, 1024, 64):
        rows.append(dict(record='clock_samples', case_id=0, offset=offset, values_ns=[1] * 64))
    for case_id, spec in results.expected_cases(header).items():
        if scope not in ('both', spec['placement']):
            rows.append(dict(record='skip', case_id=case_id, reason='placement_or_policy_unavailable'))
            continue
        rows.append(dict(record='case', case_id=case_id, server_cpu=int(spec['placement'] == 'different_cpu'), **spec))
        for index in range(3):
            rows.append(dict(record='round', case_id=case_id, round=index,
                             operations=16, elapsed_ns=16 * (cost + index - 1),
                             counters=[32] + [None] * (len(results.COUNTERS) - 1)))
        if spec['mode'] == 'rpc':
            rows.append(dict(record='latency_samples', case_id=case_id, offset=0, values_ns=list(range(1, 17))))
    return rows


def log(rows):
    return 'boot noise\n' + ''.join('IPC_BENCH ' + json.dumps(row) + '\n' for row in rows) + 'IPC_BENCH_DONE\n'


def report(cost=100):
    runs = [results.parse_log(log(records(cost=cost + index))) for index in range(3)]
    return dict(schema=results.SCHEMA, contract={'workload_sha256': 'a' * 64},
                metadata=dict(execution_kind='native', isa='aarch64', cpu_model='test', cpus=2,
                              host_identity='test', machine_id='machine', os='test'),
                runs=runs, summary=results.summarize(runs))


class IpcBenchmark(unittest.TestCase):
    def test_complete_matrix_and_counter_normalization(self):
        run = results.parse_log(log(records()))
        summary = results.summarize([run])
        self.assertEqual(len(run['cases']), 4)
        self.assertEqual(summary['cases'][0]['median_ns_per_operation'], 100)
        self.assertEqual(summary['cases'][0]['counters_per_operation']['context_switches'], 2)
        self.assertIsNone(summary['cases'][0]['counters_per_operation']['remote_wakes'])

    def test_nearest_rank_and_small_sample_tail_withholding(self):
        self.assertEqual(results.nearest_rank([1, 2, 3, 4], .5), 2)
        self.assertEqual(results.nearest_rank([1, 2, 3, 4], .99), 4)
        self.assertIsNone(results.distribution(range(9999))['p99_9_ns'])
        self.assertEqual(results.distribution(range(10000))['p99_9_ns'], 9989)

    def test_uncertainty_uses_independent_runs(self):
        runs = [results.parse_log(log(records(cost=cost))) for cost in (100, 200, 300)]
        row = results.summarize(runs)['cases'][0]
        self.assertEqual(row['run_medians_ns_per_operation'], [100, 200, 300])
        self.assertEqual(row['run_median_bootstrap_95_interval_ns'], [100, 300])
        self.assertEqual(row['independent_runs'], 3)
        self.assertIsNone(results.bootstrap_median_interval([1, 2]))

    def test_separate_boots_per_placement_are_not_extra_repetitions(self):
        runs = [results.parse_log(log(records(scope, cost)))
                for scope in ('same_cpu', 'different_cpu') for cost in (100, 200, 300)]
        summary = results.summarize(runs)
        self.assertEqual(len(summary['cases']), 4)
        self.assertTrue(all(row['independent_runs'] == 3 for row in summary['cases']))

    def test_rejects_incomplete_and_failed_capture(self):
        text = log(records())
        for broken in (text.replace('IPC_BENCH_DONE\n', ''), text + 'IPC_BENCH_DONE\n',
                       text + 'IPC_BENCH_ERROR: corruption\n', text + 'IPC_BENCH {}\n',
                       text.replace('IPC_BENCH_DONE', 'IPC_BENCH []\nIPC_BENCH_DONE')):
            with self.subTest(broken=broken[-70:]), self.assertRaises(ValueError):
                results.parse_log(broken)

    def test_rejects_bad_header_identity_hash_and_compiled_config(self):
        for field, value in [('server_process', 10), ('contract_sha256', 'unrecorded'),
                             ('samples', 0), ('include_spin', 1)]:
            rows = records(); rows[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                results.parse_log(log(rows))
        with self.assertRaisesRegex(ValueError, 'configuration mismatch'):
            results.parse_log(log(records()), {'iterations': 17})

    def test_rejects_missing_duplicate_or_overlapping_measurements(self):
        rows = records()
        for kind in ('case', 'round', 'latency_samples', 'clock_samples'):
            index = next(i for i, row in enumerate(rows) if row['record'] == kind)
            for broken in (rows[:index] + rows[index + 1:], rows[:index] + [rows[index]] + rows[index:]):
                with self.subTest(kind=kind), self.assertRaises(ValueError):
                    results.parse_log(log(broken))

    def test_rejects_false_cpu_label_zero_timing_wrong_work_and_counters(self):
        for kind, field, value in [('case', 'server_cpu', 1), ('case', 'payload_bytes', 8),
                                   ('round', 'elapsed_ns', 0), ('round', 'operations', 15),
                                   ('round', 'counters', [1]), ('latency_samples', 'offset', 1)]:
            rows = records()
            next(row for row in rows if row['record'] == kind)[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                results.parse_log(log(rows))

    def test_regression_and_valid_cross_os_comparison(self):
        old, new = report(100), report(200)
        self.assertGreater(results.compare(old, new)['cases'][0]['change_percent'], 90)
        new['metadata']['os'] = 'another OS'
        self.assertEqual(len(results.compare(old, new, cross_os=True)['cases']), 4)
        self.assertEqual(results.compare(old, old)['cases'][0]['speedup'], 1)

    def test_comparison_rejects_summary_or_compiled_hash_corruption(self):
        old = report(); new = copy.deepcopy(old)
        new['summary']['cases'][0]['median_ns_per_operation'] = 1
        with self.assertRaisesRegex(ValueError, 'stored summary'):
            results.compare(old, new)
        new = copy.deepcopy(old); new['runs'][0]['header']['contract_sha256'] = 'b' * 64
        with self.assertRaisesRegex(ValueError, 'compiled workload'):
            results.compare(old, new)

    def test_rejects_native_vs_emulated_and_different_harness(self):
        old = report()
        for field, value in [('execution_kind', 'qemu-tcg'), ('isa', 'x86_64'), ('cpu_model', 'other'),
                             ('cpus', 4), ('machine_id', 'other')]:
            new = copy.deepcopy(old); new['metadata'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                results.compare(old, new, cross_os=True)
        new = copy.deepcopy(old); new['contract']['workload_sha256'] = 'b' * 64
        with self.assertRaisesRegex(ValueError, 'contract'):
            results.compare(old, new)
        new = copy.deepcopy(old); new['metadata']['machine_id'] = None
        with self.assertRaisesRegex(ValueError, 'machine-id'):
            results.compare(old, new, cross_os=True)


if __name__ == '__main__':
    unittest.main()
