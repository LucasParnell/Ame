"""Strict IPC v1 record validation and OS-independent statistics.

Raw per-operation RTT samples describe tails; independent run medians are the
resampling units for aggregate uncertainty. Never bootstrap individual RPCs as
if they were independent machine runs, or silently subtract the clock cost.
"""
import math
import random
import statistics

SCHEMA = 'ame.ipc-benchmark/v1'
PAYLOADS = (0, 8, 48, 256, 4096)
COUNTERS = ('context_switches', 'voluntary_switches', 'involuntary_switches',
            'ipc_syscalls', 'doorbell_sleeps', 'notifications', 'yields',
            'remote_wakes', 'ipis_sent', 'timer_preempts', 'local_wakes')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, lower=0, upper=(1 << 64) - 1):
    return type(value) is int and lower <= value <= upper


def expected_cases(header):
    result = {}
    for different in range(2):
        for spin in range(2):
            if spin and (not different or not header['include_spin']):
                continue
            for mode in ('rpc', 'stream'):
                for index, size in enumerate(PAYLOADS):
                    if header['payload_mask'] & (1 << index):
                        result[len(result) + 1] = {
                            'mode': mode, 'placement': 'different_cpu' if different else 'same_cpu',
                            'wait_policy': 'spin' if spin else 'blocking', 'payload_bytes': size}
    return result


def parse_log(text, expected_config=None):
    require('IPC_BENCH_ERROR' not in text, 'peer reported IPC_BENCH_ERROR')
    require(text.splitlines().count('IPC_BENCH_DONE') == 1, 'missing/duplicate completion marker')
    header = None
    cases, skipped, clock = {}, {}, []
    done = False
    for line in text.splitlines():
        if line == 'IPC_BENCH_DONE':
            done = True
            continue
        if 'IPC_BENCH ' not in line:
            continue
        require(not done, 'benchmark records after completion')
        import json
        row = json.loads(line.split('IPC_BENCH ', 1)[1])
        require(isinstance(row, dict), 'benchmark record must be a JSON object')
        kind = row.get('record')
        if kind == 'header':
            require(header is None and not cases and not clock, 'duplicate/late header')
            require(row.get('version') == 1, 'unsupported workload version')
            bounds = {'iterations': (16, 1048576), 'warmup': (1, 1048576),
                      'samples': (16, 16384), 'rounds': (3, 31), 'window': (1, 64),
                      'payload_mask': (1, 31), 'client_cpu': (0, 65535),
                      'client_process': (1, (1 << 64) - 1), 'server_process': (1, (1 << 64) - 1)}
            for key, (low, high) in bounds.items():
                require(integer(row.get(key), low, high), f'invalid header {key}')
            require(row.get('placement_scope') in ('both', 'same_cpu', 'different_cpu'), 'unknown placement scope')
            require(type(row.get('include_spin')) is bool, 'invalid spin policy')
            digest = row.get('contract_sha256')
            require(isinstance(digest, str) and len(digest) == 64 and
                    all(char in '0123456789abcdef' for char in digest), 'missing compiled workload hash')
            require(row['client_process'] != row['server_process'], 'peers are not separate processes')
            require(isinstance(row.get('transport'), str) and row['transport'], 'missing transport')
            require(row.get('diagnostic_scope') in ('isolated_system', 'client_process'), 'unknown diagnostic scope')
            if expected_config:
                for key, value in expected_config.items():
                    require(row.get(key) == value, f'compiled/run configuration mismatch: {key}')
            header = row
            continue
        require(header is not None, 'record before header')
        case_id = row.get('case_id')
        require(integer(case_id, 0, 128), 'invalid case id')
        if kind == 'clock_samples':
            require(case_id == 0 and row.get('offset') == len(clock), 'clock chunk ordering/overlap')
            values = row.get('values_ns')
            require(isinstance(values, list) and 0 < len(values) <= 64 and
                    all(integer(value) for value in values), 'invalid clock samples')
            clock.extend(values)
            require(len(clock) <= 1024, 'too many clock samples')
        elif kind == 'skip':
            require(case_id not in cases and case_id not in skipped and case_id in expected_cases(header),
                    'duplicate/unknown skipped case')
            require(row.get('reason') == 'placement_or_policy_unavailable', 'unknown skip reason')
            skipped[case_id] = row
        elif kind == 'case':
            require(case_id not in cases and case_id not in skipped, 'duplicate case')
            expected = expected_cases(header).get(case_id)
            require(expected is not None and header['placement_scope'] in ('both', expected['placement']) and all(row.get(key) == value for key, value in expected.items()),
                    'case differs from the declared workload matrix')
            require(integer(row.get('server_cpu'), 0, 65535), 'missing server CPU')
            require((row['server_cpu'] == header['client_cpu']) == (row['placement'] == 'same_cpu'),
                    'CPU placement does not match case label')
            cases[case_id] = {'spec': row, 'rounds': {}, 'latency_samples_ns': []}
        elif kind == 'round':
            require(case_id in cases, 'round without a case')
            group = cases[case_id]
            index = row.get('round')
            require(integer(index, 0, header['rounds'] - 1) and index not in group['rounds'], 'duplicate/invalid round')
            require(row.get('operations') == header['iterations'] and integer(row.get('elapsed_ns'), 1),
                    'invalid operation count or nonpositive elapsed time')
            counters = row.get('counters')
            require(isinstance(counters, list) and len(counters) == len(COUNTERS) and
                    all(value is None or integer(value) for value in counters), 'invalid counters')
            group['rounds'][index] = {'operations': row['operations'], 'elapsed_ns': row['elapsed_ns'],
                                      'counters': dict(zip(COUNTERS, counters))}
        elif kind == 'latency_samples':
            require(case_id in cases and cases[case_id]['spec']['mode'] == 'rpc', 'RTT samples for non-RPC case')
            samples = cases[case_id]['latency_samples_ns']
            require(row.get('offset') == len(samples), 'latency chunk ordering/overlap')
            values = row.get('values_ns')
            require(isinstance(values, list) and 0 < len(values) <= 64 and
                    all(integer(value) for value in values), 'invalid latency samples')
            samples.extend(values)
            require(len(samples) <= header['samples'], 'too many latency samples')
        else:
            raise ValueError(f'unknown record type: {kind!r}')
    require(header is not None and len(clock) == 1024, 'incomplete clock calibration')
    require(set(cases) | set(skipped) == set(expected_cases(header)), 'missing matrix cases')
    require(bool(cases), 'all benchmark cases were skipped')
    for group in cases.values():
        require(len(group['rounds']) == header['rounds'], 'missing aggregate rounds')
        wanted = header['samples'] if group['spec']['mode'] == 'rpc' else 0
        require(len(group['latency_samples_ns']) == wanted, 'missing RTT samples')
        group['rounds'] = [group['rounds'][index] for index in range(header['rounds'])]
    return {'header': header, 'clock_samples_ns': clock,
            'cases': [cases[index] for index in sorted(cases)],
            'skipped': [skipped[index] for index in sorted(skipped)]}


def nearest_rank(values, quantile):
    require(bool(values) and 0 <= quantile <= 1, 'invalid quantile input')
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * quantile) - 1)]


def distribution(values):
    if not values:
        return None
    return {'count': len(values), 'min_ns': min(values), 'p50_ns': nearest_rank(values, .5),
            'p90_ns': nearest_rank(values, .9), 'p95_ns': nearest_rank(values, .95),
            'p99_ns': nearest_rank(values, .99),
            'p99_9_ns': nearest_rank(values, .999) if len(values) >= 10000 else None,
            'max_ns': max(values)}


def bootstrap_median_interval(values):
    if len(values) < 3:
        return None
    rng = random.Random(0)
    medians = [statistics.median(rng.choices(values, k=len(values))) for _ in range(2000)]
    return [nearest_rank(medians, .025), nearest_rank(medians, .975)]


def key(spec):
    return '/'.join(str(spec[field]) for field in ('mode', 'placement', 'wait_policy', 'payload_bytes'))


def summarize(runs):
    require(bool(runs), 'no runs')
    header = runs[0]['header']
    stable = ('version', 'contract_sha256', 'transport', 'iterations', 'warmup', 'samples', 'rounds', 'window',
              'payload_mask', 'include_spin', 'client_cpu', 'diagnostic_scope')
    require(all(all(run['header'][field] == header[field] for field in stable) for run in runs),
            'run configurations differ')
    expected = sorted({key(case['spec']) for run in runs for case in run['cases']})
    output = []
    for case_key in expected:
        groups = [case for run in runs for case in run['cases'] if key(case['spec']) == case_key]
        require(all(group['spec'] == groups[0]['spec'] for group in groups), 'affinity/spec changed between runs')
        run_medians = [statistics.median(row['elapsed_ns'] / row['operations'] for row in group['rounds'])
                       for group in groups]
        median = statistics.median(run_medians)
        spec = groups[0]['spec']
        latency = distribution([sample for group in groups for sample in group['latency_samples_ns']])
        output.append({'key': case_key, 'spec': spec, 'independent_runs': len(groups), 'run_medians_ns_per_operation': run_medians,
                       'median_ns_per_operation': median,
                       'run_median_bootstrap_95_interval_ns': bootstrap_median_interval(run_medians),
                       'run_min_ns': min(run_medians), 'run_max_ns': max(run_medians),
                       'run_cv': statistics.stdev(run_medians) / statistics.mean(run_medians)
                                 if len(run_medians) > 1 else None,
                       'operations_per_second': 1e9 / median,
                       'payload_bytes_per_operation': spec['payload_bytes'] * (2 if spec['mode'] == 'rpc' else 1),
                       'payload_goodput_bytes_per_second': spec['payload_bytes'] * (2 if spec['mode'] == 'rpc' else 1) * 1e9 / median,
                       'counters_per_operation': {name: statistics.median(
                           statistics.median(row['counters'][name] / row['operations'] for row in group['rounds'])
                           for group in groups) if all(row['counters'][name] is not None for group in groups
                                                       for row in group['rounds']) else None for name in COUNTERS},
                       'instrumented_rtt_ns': latency,
                       'per_run_rtt_ns': [distribution(group['latency_samples_ns']) for group in groups]})
    warnings = []
    if any(row['independent_runs'] < 3 for row in output):
        warnings.append('Fewer than three independent runs: no aggregate uncertainty interval.')
    if header['samples'] < 1000:
        warnings.append('RTT sample count is suitable for smoke checks, not stable tail estimates.')
    if header['rounds'] < 5 or header['iterations'] < 256:
        warnings.append('Short aggregate workload: smoke-check results can be dominated by startup/clock noise.')
    if any(row['run_cv'] is not None and row['run_cv'] > .1 for row in output):
        warnings.append('Some run medians vary by more than 10%; inspect interference before attributing changes.')
    return {'cases': output, 'clock_call_pair_delta_ns': distribution([sample for run in runs for sample in run['clock_samples_ns']]),
            'uncertainty_method': '2000 seeded bootstrap resamples of independent run medians; small run counts give coarse intervals',
            'quantiles': 'nearest rank; p99.9 withheld below 10000 pooled samples; RTT includes clock instrumentation',
            'warnings': warnings}


def compare(baseline, candidate, cross_os=False):
    require(baseline.get('schema') == candidate.get('schema') == SCHEMA, 'result schema mismatch')
    require(baseline['contract'] == candidate['contract'], 'workload contract/source changed; use a matching harness')
    for report in (baseline, candidate):
        require(all(run['header']['contract_sha256'] == report['contract']['workload_sha256'] for run in report['runs']),
                'report contract differs from compiled workload')
        require(report['summary'] == summarize(report['runs']), 'stored summary differs from raw measurements')
    a, b = baseline['metadata'], candidate['metadata']
    for field in ('execution_kind', 'isa', 'cpu_model', 'cpus'):
        require(a.get(field) == b.get(field), f'incompatible execution configuration: {field}')
    for field in ('memory_mib', 'entry_el', 'qemu_version', 'qemu_acceleration'):
        if not cross_os or (a.get(field) is not None and b.get(field) is not None):
            require(a.get(field) == b.get(field), f'incompatible execution configuration: {field}')
    if cross_os:
        require(a.get('machine_id') and a.get('machine_id') == b.get('machine_id'),
                'cross-OS comparison needs an explicit matching physical/emulated machine-id')
    else:
        require(a.get('machine_id') == b.get('machine_id') and a.get('host_identity') == b.get('host_identity'),
                'regression runs use different machines/hosts')
        require(a.get('affinity') == b.get('affinity'), 'regression CPU affinity/topology mismatch')
        require(baseline['runs'][0]['header']['transport'] == candidate['runs'][0]['header']['transport'],
                'regression transport mismatch; use --cross-os for different OS/transport adapters')
    for field in ('iterations', 'warmup', 'samples', 'rounds', 'window', 'payload_mask', 'include_spin'):
        require(baseline['runs'][0]['header'][field] == candidate['runs'][0]['header'][field], f'benchmark parameter mismatch: {field}')
    old = {row['key']: row for row in baseline['summary']['cases']}
    new = {row['key']: row for row in candidate['summary']['cases']}
    require(old.keys() == new.keys(), 'case sets differ (including skipped placements/policies)')
    rows = []
    for name, current in new.items():
        before = old[name]
        interval_a = before['run_median_bootstrap_95_interval_ns']
        interval_b = current['run_median_bootstrap_95_interval_ns']
        overlap = None if interval_a is None or interval_b is None else not (
            interval_a[1] < interval_b[0] or interval_b[1] < interval_a[0])
        rows.append({'key': name, 'baseline_ns': before['median_ns_per_operation'],
                     'candidate_ns': current['median_ns_per_operation'],
                     'change_percent': 100 * (current['median_ns_per_operation'] / before['median_ns_per_operation'] - 1),
                     'speedup': before['median_ns_per_operation'] / current['median_ns_per_operation'],
                     'aggregate_intervals_overlap': overlap})
    configuration_changes = {field: {'baseline': a.get(field), 'candidate': b.get(field)}
                             for field in ('build_type', 'compiler', 'development_kernel', 'benchmark_optimization')
                             if a.get(field) != b.get(field)}
    return {'schema': 'ame.ipc-comparison/v1', 'cross_os': cross_os, 'cases': rows,
            'configuration_changes': configuration_changes,
            'note': 'Compare complete workload semantics; different transports are not identical mechanisms. Overlapping/coarse intervals do not establish a performance change.'}
