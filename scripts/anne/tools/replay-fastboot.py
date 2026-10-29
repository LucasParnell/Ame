#!/usr/bin/env python3
"""Record and compare deterministic, SHA256-pinned offline Fastboot scenarios."""

import argparse
from contextlib import ExitStack
import hashlib
import importlib.util
from itertools import zip_longest
import json
from pathlib import Path
import re
import tempfile


SPEC = importlib.util.spec_from_file_location('anne_unicorn', Path(__file__).with_name('emulate-fastboot.py'))
emulation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(emulation)

FIELDS = {'schema', 'name', 'firmware_sha256', 'profile', 'entry', 'until', 'count', 'timeout_ms',
          'maps', 'writes', 'registers', 'heap_watch', 'init_bss', 'checkpoints', 'breakpoints',
          'watches', 'bitmap', 'panel', 'bytes_per_pixel', 'expected_checkpoints', 'expected_stop',
          'stop_on_oob'}


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError('addresses and sizes must be integers or integer strings')
    return int(value, 0) if isinstance(value, str) else value


def load_scenario(path):
    scenario = json.loads(path.read_text())
    if not isinstance(scenario, dict) or type(scenario.get('schema')) is not int or scenario['schema'] != 1:
        raise ValueError('scenario must be an object with schema 1')
    if scenario.keys() - FIELDS:
        raise ValueError(f'unknown scenario fields: {sorted(scenario.keys() - FIELDS)}')
    if not isinstance(scenario.get('name'), str) or not re.fullmatch(r'[A-Za-z0-9_-]+', scenario['name']):
        raise ValueError('scenario name must contain letters, digits, underscores, or hyphens')
    if not re.fullmatch(r'[0-9a-f]{64}', scenario.get('firmware_sha256', '')):
        raise ValueError('scenario must pin firmware_sha256 to 64 lowercase hex digits')
    for key in ('heap_watch', 'init_bss', 'stop_on_oob'):
        if key in scenario and type(scenario[key]) is not bool:
            raise ValueError(f'{key} must be a boolean')
    if scenario.get('expected_stop', 'until') not in ('returned', 'until', 'breakpoint'):
        raise ValueError('expected_stop must be returned, until, or breakpoint')
    return scenario


def setup(image, scenario):
    if hashlib.sha256(image).hexdigest() != scenario['firmware_sha256']:
        raise ValueError('firmware SHA256 mismatch; scenario was not executed')
    emulator = emulation.FastbootEmulator(image)
    profile = scenario.get('profile', 'raw')
    if profile == 'raw':
        entry = 0
        if scenario.get('init_bss', False):
            emulator.initialize_bss()
    elif profile in ('bmp-center', 'logo-center'):
        configure = emulator.bmp_profile if profile == 'bmp-center' else emulator.logo_center_profile
        entry = configure(tuple(scenario.get('bitmap', [2, 1])), tuple(scenario.get('panel', [8, 8])),
                          number(scenario.get('bytes_per_pixel', 1)))
    else:
        raise ValueError('scenario profile must be raw, bmp-center, or logo-center')
    for mapping in scenario.get('maps', []):
        if set(mapping) != {'address', 'size'}:
            raise ValueError('maps require exactly address and size')
        emulator.map(number(mapping['address']), number(mapping['size']))
    for write in scenario.get('writes', []):
        if set(write) != {'address', 'hex'}:
            raise ValueError('writes require exactly address and hex')
        emulator.uc.mem_write(number(write['address']), bytes.fromhex(write['hex']))
    for name, value in scenario.get('registers', {}).items():
        if name not in emulation.REGISTERS or name == 'pc':
            raise ValueError('scenario registers accept x0..x30, sp, nzcv; use entry for pc')
        value = number(value)
        if not 0 <= value < 1 << 64:
            raise ValueError('register value must fit an unsigned 64-bit integer')
        emulator.uc.reg_write(emulation.REGISTERS[name], value)
    if scenario.get('heap_watch', False) and emulator.heap is None:
        emulator.watch_heap()
    if emulator.heap:
        emulator.heap.checkpoints.update(number(pc) for pc in scenario.get('checkpoints', []))
        emulator.heap.stop_on_oob = scenario.get('stop_on_oob', False)
    elif scenario.get('checkpoints'):
        raise ValueError('heap checkpoints require heap_watch or a logo profile')
    emulator.assumptions.append('All additional maps, writes and initial registers are recorded in scenario.json.')
    return emulator, number(scenario.get('entry', entry))


def compare_jsonl(expected_path, actual_path):
    """Stream to the first mismatch; retain only a small common prefix context."""
    context = []
    with expected_path.open() as expected, actual_path.open() as actual:
        for line, (before, after) in enumerate(zip_longest(expected, actual), 1):
            before, after = (json.loads(s) if s is not None else None for s in (before, after))
            if before != after:
                fields = sorted(k for k in (before or {}).keys() | (after or {}).keys()
                                if (before or {}).get(k) != (after or {}).get(k))
                steps = [s['step'] for s in (before, after) if s is not None and 'step' in s]
                return {'file': actual_path.name, 'line': line, 'step': min(steps) if steps else None,
                        'changed_fields': fields, 'expected': before, 'actual': after,
                        'preceding_context': context}
            context = (context + [before])[-8:]
    return None


def compare_replays(expected, actual):
    differences = []
    for name in ('trace.jsonl', 'execution.jsonl', 'events.jsonl'):
        difference = compare_jsonl(expected / name, actual / name)
        if difference:
            differences.append(difference)
    first = min(differences, key=lambda d: d['step'] if d['step'] is not None else float('inf'), default=None)
    # Include final state: a last-instruction store can differ without a following code hook.
    report_before = json.loads((expected / 'report.json').read_text())
    report_after = json.loads((actual / 'report.json').read_text())
    changed = [key for key in sorted(report_before.keys() | report_after.keys())
               if report_before.get(key) != report_after.get(key)]
    configuration_changed = json.loads((expected / 'scenario.json').read_text()) != json.loads(
        (actual / 'scenario.json').read_text())
    return {'matched': not differences and not changed and not configuration_changed,
            'configuration_changed': configuration_changed,
            'first_execution_divergence': first, 'stream_differences': differences,
            'changed_report_fields': changed}


def record_snapshot(emulator, output, snapshots, label, state, watches=()):
    """Persist the used arena and watched buffers while execution is paused."""
    record = {'label': label, **state, 'memory': []}
    ranges = []
    if emulator.heap:
        lo, size = emulator.heap.arena
        cursor = state.get('cursor')
        if cursor is not None and lo <= cursor <= lo + size:
            ranges.append((lo, cursor - lo, 'heap'))
        else:
            record['heap_bytes_unavailable'] = 'allocator cursor is not initialized inside the monitored arena'
    if 'clear' in state:
        clear = state['clear']
        if 0 < clear['size'] <= 16 << 20:
            ranges.append((clear['destination'], clear['size'], 'clear-destination'))
    for index, (address, size) in enumerate(watches):
        if size <= 16 << 20:
            ranges.append((address, size, f'watch-{index}'))
        else:
            record['memory'].append({'address': hex(address), 'size': size,
                                     'error': 'watch snapshot exceeds 16 MiB limit'})
    for address, size, kind in ranges:
        name = f'snapshot-{len(snapshots):02d}-{kind}.bin'
        try:
            data = bytes(emulator.uc.mem_read(address, size)) if size else b''
            (output / name).write_bytes(data)
            record['memory'].append({'address': hex(address), 'size': size, 'file': name,
                                     'sha256': hashlib.sha256(data).hexdigest()})
        except emulation.unicorn.UcError as error:
            record['memory'].append({'address': hex(address), 'size': size, 'error': str(error)})
    snapshots.append(record)


def summary_for(scenario, report):
    checkpoints = report.get('heap', {}).get('checkpoint_snapshots', [])
    at = {item['pc']: item for item in checkpoints}
    expected = [number(pc) for pc in scenario.get('expected_checkpoints', [])]
    missing = [hex(pc) for pc in expected if pc not in at]
    observed_order = [item['pc'] for item in checkpoints if item['pc'] in expected]
    order_matched = observed_order == expected
    expected_stop = scenario.get('expected_stop', 'until')
    passed = not missing and order_matched and report['stop_reason'] == expected_stop
    passed = passed and not any(report['memory_diagnostics'].values())
    passed = passed and not any('error' in memory for snapshot in report.get('snapshots', [])
                                for memory in snapshot['memory'])
    boot = report.get('boot_progress', {})
    return {'scenario': scenario['name'], 'firmware': 'SHA256 verified',
            'module_initialization': boot.get('module_initialization', 'not-observed'),
            'logo_processing': boot.get('hisi_display_logo', 'not-reached'),
            'centering': boot.get('centering', 'not-reached'),
            'heap': {'initial_cursor': report.get('initial_heap', {}).get('cursor')
                                      if report.get('initial_heap', {}).get('cursor_initialized') else None,
                     'initial_cursor_raw': report.get('initial_heap', {}).get('cursor'),
                     'cursor_before_logo': at.get(emulation.BMP_CENTER, {}).get('cursor'),
                     'cursor_after_logo': at.get(0x4f028, {}).get('cursor'),
                     'allocations': len(report.get('heap', {}).get('allocations', [])),
                     'pointer_bearing': len({p['allocation'] for p in report.get('heap', {}).get('pointer_slots', [])
                                             if p['allocation'] is not None})},
            'memory_diagnostics': report['memory_diagnostics'], 'missing_checkpoints': missing,
            'checkpoint_order_matched': order_matched,
            'stop_reason': 'expected checkpoint' if passed and report['stop_reason'] in ('until', 'breakpoint')
                           else report['stop_reason'],
            'execution_passed': passed,
            'normal_boot_milestone_complete': passed and boot.get('module_initialization') == 'completed'
                                              and boot.get('hisi_display_logo') == 'reached'
                                              and all(pc in at for pc in emulation.LOGO_CHECKPOINTS)
                                              and bool(boot.get('modules'))
                                              and all(m['status'] == 'returned' for m in boot['modules']),
            **({'failure_context': report['failure_context']} if 'failure_context' in report else {})}


def replay(image_path, scenario_path, output=None, compare=None):
    scenario = load_scenario(scenario_path)
    image = image_path.read_bytes()
    emulator, entry = setup(image, scenario)
    if output is None:
        output = Path(tempfile.mkdtemp(prefix='ame-fastboot-replay-'))
    else:
        output = output.expanduser().resolve()
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / 'scenario.json').write_text(json.dumps(scenario, indent=2) + '\n')
    snapshots = []
    watches = [(number(r['address']), number(r['size'])) for r in scenario.get('watches', [])]
    if emulator.heap:
        record_snapshot(emulator, output, snapshots, 'initial', emulator.heap.snapshot(), watches)
    elif watches:
        record_snapshot(emulator, output, snapshots, 'initial', {}, watches)

    def checkpoint(snapshot):
        if len(snapshots) < 65:
            record_snapshot(emulator, output, snapshots, snapshot['label'], snapshot, watches)

    with ExitStack() as stack:
        streams = {name: stack.enter_context((output / name).open('w'))
                   for name in ('trace.jsonl', 'events.jsonl', 'execution.jsonl')}
        report = emulator.run(entry, until=number(scenario['until']) if 'until' in scenario else None,
                              count=number(scenario.get('count', 100000)),
                              timeout_ms=number(scenario.get('timeout_ms', 5000)),
                              trace=streams['trace.jsonl'], events=streams['events.jsonl'],
                              breakpoints=[number(pc) for pc in scenario.get('breakpoints', [])],
                              watches=watches,
                              observation_stream=streams['execution.jsonl'], trace_registers=True,
                              checkpoint_callback=checkpoint)
    if emulator.heap:
        record_snapshot(emulator, output, snapshots, 'final', emulator.heap.snapshot(), watches)
        emulation.save_allocations(output, emulator.heap.allocations)
    elif watches:
        record_snapshot(emulator, output, snapshots, 'final', {}, watches)
    report['snapshots'] = snapshots
    report['scenario'] = scenario['name']
    report['source_image_unchanged'] = hashlib.sha256(image_path.read_bytes()).hexdigest() == emulator.sha256
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    summary = summary_for(scenario, report)
    if not report['source_image_unchanged']:
        summary['execution_passed'] = False
        summary['normal_boot_milestone_complete'] = False
    if compare is not None:
        comparison = compare_replays(compare.expanduser().resolve(), output)
        (output / 'comparison.json').write_text(json.dumps(comparison, indent=2) + '\n')
        summary['replay_matched'] = comparison['matched']
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return output, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=emulation.DEFAULT_IMAGE)
    parser.add_argument('--scenario', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--compare', type=Path, help='previous replay directory; fail on first divergence')
    args = parser.parse_args()
    try:
        output, summary = replay(args.image.expanduser(), args.scenario.expanduser(), args.output, args.compare)
        print(json.dumps(summary, indent=2))
        print(f'Results: {output}')
        return 0 if summary['execution_passed'] and summary.get('replay_matched', True) else 1
    except (OSError, ValueError, TypeError, KeyError, emulation.unicorn.UcError) as error:
        parser.exit(2, f'fastboot replay: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
