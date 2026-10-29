"""Separate shared-loan experiment: no mixing with copied IPC v1 reports."""
import json
import statistics


def require(ok, why):
    if not ok:
        raise ValueError(why)


def parse_log(text):
    require('IPC_LOAN_ERROR' not in text, 'peer failed payload/ownership validation')
    runs, current = [], None
    for line in text.splitlines():
        if line == 'IPC_LOAN_DONE':
            require(current is not None, 'completion without header')
            h, rows = current['header'], current['rounds']
            expected = {(t, m, r) for t in ('copied_ring', 'shared_loan')
                        for m in ('rpc', 'stream') for r in range(h['rounds'])}
            require(set(rows) == expected, 'incomplete case/round matrix')
            current['rounds'] = list(rows.values())
            runs.append(current)
            current = None
            continue
        if 'IPC_LOAN ' not in line:
            continue
        row = json.loads(line.split('IPC_LOAN ', 1)[1])
        require(isinstance(row, dict), 'record must be an object')
        if row.get('record') == 'header':
            require(current is None, 'duplicate/unfinished header')
            require(row.get('schema') == 'ame.ipc-loans/v1' and row.get('payload_bytes') == 4096,
                    'unknown experiment contract')
            for key, lower, upper in (('iterations', 1, 1048576), ('warmup', 1, 1048576),
                                      ('rounds', 3, 31), ('window', 1, 32), ('server_cpu', 0, 7)):
                require(type(row.get(key)) is int and lower <= row[key] <= upper, f'invalid {key}')
            require(row.get('buffers') == 32 and type(row.get('shared_system')) is bool,
                    'invalid pool/scope')
            current = {'header': row, 'rounds': {}}
        else:
            require(current is not None and row.get('record') == 'round', 'unknown/late record')
            require(type(row.get('round')) is int and 0 <= row['round'] < current['header']['rounds'],
                    'invalid round index')
            require(type(row.get('operations')) is int and row['operations'] == current['header']['iterations']
                    and type(row.get('elapsed_ns')) is int and row['elapsed_ns'] > 0, 'invalid timing/count')
            require(row.get('transport') in ('copied_ring', 'shared_loan') and row.get('mode') in ('rpc', 'stream'),
                    'invalid case')
            key = (row['transport'], row['mode'], row['round'])
            require(key not in current['rounds'], 'duplicate round')
            current['rounds'][key] = row
    require(current is None and runs, 'missing completion or empty capture')
    require(all(run['header'] == runs[0]['header'] for run in runs), 'run configurations differ')
    return runs


def summarize(runs):
    output = []
    for mode in ('rpc', 'stream'):
        values = {}
        for transport in ('copied_ring', 'shared_loan'):
            medians = [statistics.median(r['elapsed_ns'] / r['operations'] for r in run['rounds']
                                         if r['transport'] == transport and r['mode'] == mode) for run in runs]
            values[transport] = {'median_ns': statistics.median(medians), 'run_medians_ns': medians}
        output.append({'mode': mode, **values,
                       'loan_time_reduction_percent': 100 * (1 - values['shared_loan']['median_ns'] /
                                                             values['copied_ring']['median_ns'])})
    return output
