#!/usr/bin/env python3
"""Reproduce the isolated OEMINFO BMP parser and centering investigation.

This tool never attempts to boot Fastboot.  It runs two small, real instruction
paths from the hash-pinned image: the BMP-header continuation ending at the
pixel-pointer store and the centering function.  All BMPs, display state, and
stacks in this experiment are synthetic and recorded as such in the output.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile


SPEC = importlib.util.spec_from_file_location(
    'anne_unicorn', Path(__file__).with_name('emulate-fastboot.py'))
emulation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(emulation)


PARSER_ENTRY = 0x55D28
PARSER_PIXEL_POINTER = 0x55D7C
PARSER_RESULT_STORE_STOP = 0x55BD8
CENTER = 0x4EBEC
CLEAR_SCRATCH = 0x4EC98
COPY_TO_SCRATCH = 0x4ECB0
CLEAR_DESTINATION = 0x4ECDC
COPY_CENTERED_ROW = 0x4ED34
ALLOCATE_SCRATCH = 0x4EF18
INPUT_BASE = 0x36000000
INPUT_SIZE = 0x20000
PANEL = (8, 8)
BYTES_PER_PIXEL = 2


def plain(value):
    """Convert report data to JSON-friendly values while retaining integer ranges."""
    if isinstance(value, dict):
        return {key: plain(item) for key, item in value.items()}
    if isinstance(value, list):
        return [plain(item) for item in value]
    return value


def read_u64(uc, address):
    return struct.unpack('<Q', bytes(uc.mem_read(address, 8)))[0]


def range_dict(start, size):
    return {'start': start, 'end': start + size, 'size': size}


def relation(operation, allocation):
    """Compare a recorded C-library operation to the allocation it touches."""
    item = operation['destination']
    if allocation is None:
        return {'allocation_observed': False, 'status': 'no-observed-allocation'}
    return {
        'allocation_observed': True,
        'allocation_id': allocation['id'],
        'requested': range_dict(allocation['address'], allocation['size']),
        'reserved': range_dict(allocation['address'], allocation['reserved_end'] - allocation['address']),
        'within_requested': item['start'] >= allocation['address'] and item['end'] <= allocation['requested_end'],
        'within_reservation': item['start'] >= allocation['address'] and item['end'] <= allocation['reserved_end'],
        'bytes_past_requested': max(0, item['end'] - allocation['requested_end']),
        'bytes_past_reservation': max(0, item['end'] - allocation['reserved_end']),
    }


def make_bmp(width, height, seed):
    """Construct a synthetic 16-bpp BI_RGB BMP, with deterministic payload bytes."""
    if not (0 < width <= PANEL[0] and 0 < height <= PANEL[1]):
        raise ValueError('experiment BMP dimensions must fit the synthetic panel')
    pixel_bytes = width * height * BYTES_PER_PIXEL
    header = bytearray(54)
    header[0:2] = b'BM'
    struct.pack_into('<I', header, 2, len(header) + pixel_bytes)
    struct.pack_into('<I', header, 10, len(header))
    struct.pack_into('<I', header, 14, 40)
    struct.pack_into('<i', header, 18, width)
    struct.pack_into('<i', header, 22, height)
    struct.pack_into('<H', header, 26, 1)
    struct.pack_into('<H', header, 28, 16)
    struct.pack_into('<I', header, 34, pixel_bytes)
    pixels = bytes((seed + index) & 0xff for index in range(pixel_bytes))
    return bytes(header) + pixels


def write_fixture(emulator, index, width, height, seed):
    """Write one explicit synthetic OEMINFO-like resource slot to mapped RAM."""
    slot = INPUT_BASE + index * 0x10000
    bmp_offset = 0x200
    bmp = make_bmp(width, height, seed)
    emulator.uc.mem_write(slot, bytes([0xa5]) * 0x1000)
    emulator.uc.mem_write(slot, struct.pack('<I', bmp_offset))
    emulator.uc.mem_write(slot + bmp_offset, bmp)
    return {
        'kind': 'synthetic-input',
        'slot': slot,
        'resource_offset_storage': slot,
        'bmp_offset': bmp_offset,
        'bmp_header': slot + bmp_offset,
        'pixel_start': slot + bmp_offset + 54,
        'pixel_size': width * height * BYTES_PER_PIXEL,
        'width': width,
        'height': height,
        'bit_count': 16,
        'sha256': hashlib.sha256(bmp).hexdigest(),
    }


def configure_emulator(image):
    emulator = emulation.FastbootEmulator(image.read_bytes())
    emulator.require_stock()
    emulator.initialize_bss()
    emulator.map(emulation.HEAP_BASE, emulation.HEAP_SIZE, 'native allocator arena')
    emulator.map(INPUT_BASE, INPUT_SIZE, 'synthetic OEMINFO BMP fixtures')
    # The centering routine obtains panel dimensions through these real globals.
    emulator.uc.mem_write(emulation.SCRATCH + 0x148, struct.pack('<Q', emulation.SCRATCH + 0x200))
    emulator.uc.mem_write(emulation.SCRATCH + 0x204, struct.pack('<II', *PANEL))
    display_cell = read_u64(emulator.uc, 0x291A88)
    emulator.uc.mem_write(display_cell, struct.pack('<Q', emulation.SCRATCH + 0x100))
    log_level = read_u64(emulator.uc, 0x291740)
    emulator.uc.mem_write(log_level, struct.pack('<I', 0))
    emulator.assumptions.extend((
        'Focused OEMINFO BMP investigation only; no Fastboot boot path is executed.',
        'BMP headers/pixels, display structure, stack, and allocator invocation boundary are synthetic fixtures.',
        'The parser begins at 0x55d28, after earlier OEMINFO resource discovery, and stops before logging at 0x55bd8.',
        'The centering routine, allocator, clears, and copies are instructions from the verified firmware image.',
        'A synthetic canary experiment demonstrates write reachability only and is not evidence of a live pointer overwrite.',
    ))
    emulator.watch_heap(checkpoints=(CENTER, CLEAR_SCRATCH, COPY_TO_SCRATCH,
                                     CLEAR_DESTINATION, COPY_CENTERED_ROW, ALLOCATE_SCRATCH))
    return emulator


def run_parser(emulator, fixture, name, output):
    """Run genuine parsing instructions through its result store, avoiding logger state."""
    stack = emulation.STACK + emulation.STACK_SIZE - 0x400
    rect = emulation.SCRATCH + 0x400
    out_pointer = emulation.SCRATCH + 0x480
    emulator.uc.mem_write(stack, bytes(0x100))
    emulator.uc.mem_write(stack + 8, struct.pack('<Q', emulation.RETURN))
    emulator.uc.mem_write(stack + 0x70, struct.pack('<I', fixture['bmp_offset']))
    emulator.uc.mem_write(rect, bytes(0x20))
    emulator.uc.mem_write(out_pointer, bytes(8))
    for register, value in {
            'sp': stack, 'x29': stack, 'x19': 0x130000, 'x20': rect, 'x21': 0,
            'x22': out_pointer, 'x25': fixture['slot'], 'x30': emulation.RETURN}.items():
        emulator.uc.reg_write(emulation.REGISTERS[register], value)
    trace_path = output / f'{name}-parser-instructions.jsonl'
    observation_path = output / f'{name}-parser-memory.jsonl'
    # Parser stack writes are part of the synthetic calling frame, not heap provenance.
    saved_heap, emulator.heap = emulator.heap, None
    try:
        with trace_path.open('w') as trace, observation_path.open('w') as observations:
            report = emulator.run(PARSER_ENTRY, count=10000, timeout_ms=3000, trace=trace,
                                  breakpoints=(PARSER_RESULT_STORE_STOP,),
                                  watches=((fixture['slot'], 0x1000), (rect, 0x20), (out_pointer, 8)),
                                  observation_stream=observations, trace_registers=True)
    finally:
        emulator.heap = saved_heap
    pointer = read_u64(emulator.uc, out_pointer)
    left, top, right, bottom, bpp = struct.unpack('<IIIII', bytes(emulator.uc.mem_read(rect, 20)))
    pcs = {int(item['pc'], 16) for item in report['diagnostics']['coverage']['instructions']}
    if report['stop_reason'] != 'breakpoint' or PARSER_PIXEL_POINTER not in pcs:
        raise RuntimeError(f'parser did not reach {PARSER_PIXEL_POINTER:#x}: {report["stop_reason"]}')
    parsed = {'pixel_pointer': pointer, 'width': right - left + 1, 'height': bottom - top + 1,
              'bit_count': bpp * 8, 'rect': {'address': rect, 'left': left, 'top': top,
                                               'right': right, 'bottom': bottom,
                                               'bytes_per_pixel': bpp}}
    if (pointer != fixture['pixel_start'] or parsed['width'] != fixture['width']
            or parsed['height'] != fixture['height'] or bpp != BYTES_PER_PIXEL):
        raise RuntimeError(f'parser result does not match fixture: {parsed}')
    return {'fixture': fixture, 'entry': PARSER_ENTRY, 'stop_before': PARSER_RESULT_STORE_STOP,
            'report_file': trace_path.name, 'memory_file': observation_path.name,
            'report': report, 'parsed': parsed}


def branch_path(trace_path):
    """Extract every executed branch/call/return from the complete instruction trace."""
    instructions = [json.loads(line) for line in trace_path.read_text().splitlines() if line]
    result = []
    for index, item in enumerate(instructions):
        mnemonic = item['instruction'].split(' ', 1)[0]
        if mnemonic.startswith('b') or mnemonic in ('bl', 'blr', 'br', 'ret'):
            next_pc = int(instructions[index + 1]['pc'], 16) if index + 1 < len(instructions) else None
            pc = int(item['pc'], 16)
            result.append({'pc': pc, 'instruction': item['instruction'], 'next_pc': next_pc,
                           'conditional': mnemonic not in ('b', 'bl', 'blr', 'br', 'ret'),
                           'taken': None if next_pc is None or mnemonic in ('bl', 'blr', 'br', 'ret')
                           else next_pc != pc + 4})
    return result


def memory_accesses(memory_path):
    events = [json.loads(line) for line in memory_path.read_text().splitlines() if line]
    return [{'kind': item['kind'], 'pc': int(item['pc'], 16), 'range': range_dict(int(item['address'], 16), item['size']),
             'watches': item['watches']} for item in events if item['kind'] in ('memory-read', 'memory-write')]


def new_heap_events(heap, start_sequence):
    return [event for event in heap.events if event['sequence'] > start_sequence]


def find_operation(events, callsite):
    return next((event for event in events if event['kind'] == 'memory-operation'
                 and event['callsite'] == callsite), None)


def allocation_after(events, previous_count):
    returned = [event for event in events if event['kind'] == 'allocation-return']
    return returned[-1] if returned and returned[-1]['id'] > previous_count else None


def pointer_updates(events, allocation, before_slots):
    """Only firmware instruction stores count as observed live pointer storage."""
    starts = set(before_slots)
    writes = [event for event in events if event['kind'] == 'pointer-sized-write'
              and event.get('provenance') == 'firmware-instruction']
    inside = [event for event in writes if allocation and allocation['address'] <= event['address'] < allocation['reserved_end']]
    preexisting_changed = [event for event in inside if event['address'] in starts]
    return {'firmware_pointer_writes': writes,
            'firmware_pointer_writes_in_affected_allocation': inside,
            'firmware_pointer_writes_to_scratch_global': [event for event in writes
                                                           if event['address'] == emulation.BMP_SCRATCH],
            'preexisting_firmware_pointer_storage_modified': preexisting_changed,
            'live_pointer_storage_modified': bool(preexisting_changed)}


def center(emulator, parser, name, call_number, output):
    parsed = parser['parsed']
    rect = parsed['rect']['address']
    pixel_extent = range_dict(parsed['pixel_pointer'], parsed['width'] * parsed['height'] * BYTES_PER_PIXEL)
    stack = emulation.STACK + emulation.STACK_SIZE - 0x800 - call_number * 0x200
    emulator.uc.mem_write(stack, bytes(0x100))
    emulator.uc.reg_write(emulation.REGISTERS['sp'], stack)
    emulator.uc.reg_write(emulation.REGISTERS['x0'], rect)
    emulator.uc.reg_write(emulation.REGISTERS['x1'], parsed['pixel_pointer'])
    emulator.uc.reg_write(emulation.REGISTERS['x30'], emulation.RETURN)
    heap = emulator.heap
    before_sequence = heap.event_count
    before_allocations = len(heap.allocations)
    before_scratch = read_u64(emulator.uc, emulation.BMP_SCRATCH)
    before_slots = {slot['address']: slot['value'] for slot in heap.snapshot()['pointer_slots']
                    if slot['provenance'] == 'firmware-instruction'}
    trace_path = output / f'{name}-center-{call_number}-instructions.jsonl'
    memory_path = output / f'{name}-center-{call_number}-memory.jsonl'
    event_path = output / f'{name}-center-{call_number}-heap.jsonl'
    watches = ((emulation.HEAP_BASE, 0x1000), (emulation.BMP_SCRATCH, 8),
               (parsed['pixel_pointer'], PANEL[0] * PANEL[1] * BYTES_PER_PIXEL), (rect, 0x20))
    with trace_path.open('w') as trace, memory_path.open('w') as observations, event_path.open('w') as events:
        report = emulator.run(CENTER, count=150000, timeout_ms=5000, trace=trace, events=events,
                              watches=watches, observation_stream=observations, trace_registers=True)
    if report['stop_reason'] != 'returned':
        raise RuntimeError(f'centering call {call_number} stopped as {report["stop_reason"]}')
    events = new_heap_events(heap, before_sequence)
    scratch = read_u64(emulator.uc, emulation.BMP_SCRATCH)
    allocation = allocation_after(events, before_allocations)
    if allocation is None and scratch:
        allocation = next((item for item in reversed(heap.allocations)
                           if item.get('address') == scratch), None)
    operations = {hex(pc): find_operation(events, pc) for pc in
                  (CLEAR_SCRATCH, COPY_TO_SCRATCH, CLEAR_DESTINATION, COPY_CENTERED_ROW)}
    allocation_comparisons = {label: relation(operation, allocation) for label, operation in operations.items()
                              if operation is not None and operation['destination']['touches_heap_arena']}
    dest_clear = operations.get(hex(CLEAR_DESTINATION))
    output_clear_vs_fixture = None
    if dest_clear:
        destination = dest_clear['destination']
        output_clear_vs_fixture = {'declared_synthetic_pixel_extent': pixel_extent,
                                   'clear_destination': range_dict(destination['start'], destination['size']),
                                   'exceeds_declared_pixel_bytes': destination['end'] > pixel_extent['end'],
                                   'backing_capacity': 'unknown: fixture mapping is synthetic, not a firmware allocation'}
    allocation_this_call = allocation_after(events, before_allocations)
    requested_scratch_bytes = parsed['width'] * parsed['height'] * BYTES_PER_PIXEL
    return {
        'call': call_number, 'parser_result': parser['parsed'], 'before_scratch_pointer': before_scratch,
        'after_scratch_pointer': scratch,
        'scratch_reused': bool(before_scratch and before_scratch == scratch and allocation_this_call is None),
        'requested_scratch_bytes': requested_scratch_bytes,
        'allocation_request_this_call': None if allocation_this_call is None else allocation_this_call['size'],
        'allocation_from_this_call': allocation_this_call,
        'active_scratch_allocation': allocation,
        'actual_capacity': None if allocation is None else allocation['reserved_end'] - allocation['address'],
        'requested_allocation_size': None if allocation is None else allocation['size'],
        'memory_operations': operations, 'allocation_comparisons': allocation_comparisons,
        'display_clear_vs_synthetic_input': output_clear_vs_fixture,
        'branch_path': branch_path(trace_path), 'memory_accesses': memory_accesses(memory_path),
        'pointer_storage': pointer_updates(events, allocation, before_slots),
        'heap_events': events, 'report': report,
        'artifacts': {'instructions': trace_path.name, 'memory': memory_path.name, 'heap_events': event_path.name},
    }


def synthetic_canary(emulator, name, output):
    """Allocate with real allocator, then host-write a labelled synthetic pointer canary."""
    stack = emulation.STACK + emulation.STACK_SIZE - 0x1200
    emulator.uc.mem_write(stack, bytes(0x80))
    emulator.uc.reg_write(emulation.REGISTERS['sp'], stack)
    emulator.uc.reg_write(emulation.REGISTERS['x0'], 32)
    emulator.uc.reg_write(emulation.REGISTERS['x30'], emulation.RETURN)
    trace_path = output / f'{name}-synthetic-canary-allocator.jsonl'
    event_path = output / f'{name}-synthetic-canary-allocator-events.jsonl'
    before_events = emulator.heap.event_count
    with trace_path.open('w') as trace, event_path.open('w') as events:
        report = emulator.run(emulation.ALLOCATOR, count=10000, timeout_ms=3000, trace=trace, events=events,
                              trace_registers=True)
    if report['stop_reason'] != 'returned':
        raise RuntimeError('real allocator did not return for synthetic canary')
    allocation = allocation_after(new_heap_events(emulator.heap, before_events), len(emulator.heap.allocations) - 1)
    canary = emulation.HEAP_BASE + 0x300
    emulator.uc.mem_write(allocation['address'], struct.pack('<Q', canary))
    return {'kind': 'synthetic-host-pointer-canary', 'provenance': 'synthetic-host-write',
            'allocation': allocation, 'storage': allocation['address'], 'initial_value': canary,
            'initial_bytes': bytes(emulator.uc.mem_read(allocation['address'], 8)).hex(),
            'allocator_report': report, 'artifact': trace_path.name}


def run_experiment(image, output, name, dimensions, with_canary=False):
    emulator = configure_emulator(image)
    calls, parser_runs, canary = [], [], None
    for index, (width, height) in enumerate(dimensions):
        fixture = write_fixture(emulator, index, width, height, seed=0x20 + index * 0x40)
        parser = run_parser(emulator, fixture, f'{name}-{index + 1}', output)
        parser_runs.append({'fixture': fixture, 'parsed': parser['parsed'],
                            'branch_path': branch_path(output / parser['report_file']),
                            'memory_accesses': memory_accesses(output / parser['memory_file']),
                            'report': parser['report'],
                            'artifacts': {'instructions': parser['report_file'], 'memory': parser['memory_file']}})
        calls.append(center(emulator, parser, name, index + 1, output))
        if index == 0 and with_canary:
            canary = synthetic_canary(emulator, name, output)
    if with_canary:
        final = bytes(emulator.uc.mem_read(canary['storage'], 8)).hex()
        canary['final_bytes'] = final
        canary['modified_by_following_centering'] = final != canary['initial_bytes']
        canary['conclusion'] = ('Synthetic canary overwritten by the following real centering call; '
                                'this demonstrates reachability only, not a live pointer overwrite.')
    return {'name': name, 'dimensions': [{'width': w, 'height': h} for w, h in dimensions],
            'synthetic_input': True, 'parser_runs': parser_runs, 'centering_calls': calls,
            'synthetic_canary': canary if with_canary else None,
            'final_heap': emulator.heap.snapshot()}


def file_hashes(output):
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(output.iterdir()) if path.is_file() and path.name != 'trace.json'}


def findings(trace):
    lines = ['# OEMINFO BMP isolated Unicorn findings', '',
             f"Firmware SHA-256 verified: `{trace['firmware']['sha256']}`.", '',
             'This replay executes real instructions at the parser continuation, centering routine, '
             'allocator, memory-clear, and copy callsites. BMP data, display state, stacks, and the '
             'optional canary are synthetic fixtures.', '', '## Observations', '']
    for experiment in trace['experiments']:
        first, second = experiment['centering_calls']
        parsed = experiment['parser_runs'][0]['parsed']
        lines.append(f"- `{experiment['name']}` parsed `{parsed['width']}x{parsed['height']}` 16-bpp BMP data at "
                     f"`{parsed['pixel_pointer']:#x}` through `0x55d7c`.")
        allocation = first['active_scratch_allocation']
        lines.append(f"  First centering requested `{first['requested_allocation_size']}` bytes; allocator reserved "
                     f"`{first['actual_capacity']}` bytes at `{allocation['address']:#x}`.")
        lines.append(f"  Second centering scratch reuse: `{second['scratch_reused']}`. "
                     f"Observed allocation-call this second invocation: `{second['allocation_from_this_call'] is not None}`.")
        for callsite in (hex(CLEAR_SCRATCH), hex(COPY_TO_SCRATCH)):
            comparison = second['allocation_comparisons'].get(callsite)
            if comparison:
                lines.append(f"  `{callsite}` destination is within requested allocation: "
                             f"`{comparison['within_requested']}`; within reservation: "
                             f"`{comparison['within_reservation']}`; bytes past reservation: "
                             f"`{comparison['bytes_past_reservation']}`.")
        clear = second['display_clear_vs_synthetic_input']
        if clear:
            lines.append(f"  `0x4ecdc` clear exceeds the declared synthetic pixel extent: "
                         f"`{clear['exceeds_declared_pixel_bytes']}`. Its backing allocation capacity is unknown.")
            copy_row = second['memory_operations'].get(hex(COPY_CENTERED_ROW))
            if copy_row:
                destination = copy_row['destination']
                source = copy_row['source']
                lines.append(f"  Second-call `0x4ecdc` destination is "
                             f"`[{clear['clear_destination']['start']:#x}, {clear['clear_destination']['end']:#x})`; "
                             f"first observed `0x4ed34` row copy is "
                             f"`[{source['start']:#x}, {source['end']:#x})` to "
                             f"`[{destination['start']:#x}, {destination['end']:#x})`.")
        live = any(call['pointer_storage']['live_pointer_storage_modified'] for call in experiment['centering_calls'])
        globals_written = first['pointer_storage']['firmware_pointer_writes_to_scratch_global']
        if globals_written:
            pointer = globals_written[-1]
            lines.append(f"  Firmware stored scratch pointer `{pointer['value']:#x}` at global "
                         f"`{pointer['address']:#x}` (`0x4ef1c`); that storage is outside the allocation.")
        lines.append(f"  Observed live firmware pointer storage modified inside the affected allocation: `{live}`.")
        if experiment['synthetic_canary']:
            canary = experiment['synthetic_canary']
            lines.append(f"  Synthetic host-pointer canary changed: `{canary['modified_by_following_centering']}`. "
                         'It is explicitly not evidence of a live pointer overwrite.')
    lines.extend(['', '## Conclusion', '',
                  'The trace establishes an out-of-reservation scratch-buffer write for the controlled '
                  'small-to-large reuse input. It does not establish a live pointer overwrite: no such '
                  'firmware-written pointer storage was observed in the affected allocation. The complete '
                  'branch paths, memory watches, allocator events, and exact synthetic provenance are in `trace.json`.'])
    return '\n'.join(lines) + '\n'


def analyse(image, output):
    image = image.expanduser().resolve()
    if not image.is_file():
        raise ValueError(f'firmware image not found: {image}')
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    if digest != emulation.STOCK_SHA256:
        raise ValueError(f'firmware hash does not match required stock image: {digest}')
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    scenario = {'schema': 1, 'name': 'oeminfo_bmp_memory_investigation',
                'firmware_sha256': digest, 'firmware_base': 0,
                'scope': 'isolated parser continuation and centering only; no boot emulation',
                'addresses': {'parser_entry': PARSER_ENTRY, 'pixel_pointer': PARSER_PIXEL_POINTER,
                              'parser_stop': PARSER_RESULT_STORE_STOP, 'centering': CENTER,
                              'clear_scratch': CLEAR_SCRATCH, 'copy_to_scratch': COPY_TO_SCRATCH,
                              'clear_destination': CLEAR_DESTINATION, 'copy_centered_row': COPY_CENTERED_ROW,
                              'allocator': emulation.ALLOCATOR},
                'panel': {'width': PANEL[0], 'height': PANEL[1], 'bytes_per_pixel': BYTES_PER_PIXEL},
                'experiments': [{'name': 'small_then_large', 'dimensions': ['2x1', '8x8']},
                                {'name': 'large_then_small', 'dimensions': ['8x8', '2x1']},
                                {'name': 'small_then_large_synthetic_canary', 'dimensions': ['2x1', '8x8'],
                                 'canary': 'synthetic host-written pointer after real allocator call'}]}
    (output / 'scenario.json').write_text(json.dumps(scenario, indent=2) + '\n')
    trace = {'schema': 1, 'firmware': {'path': str(image), 'sha256': digest, 'base': 0,
                                        'verified_stock_image': True},
             'scenario_file': 'scenario.json', 'scope': scenario['scope'],
             'provenance': {'firmware_execution': 'real instructions from verified image',
                            'inputs': 'synthetic and explicitly tagged per fixture',
                            'pointer_rule': 'only firmware-instruction pointer stores can be live pointer evidence'},
             'experiments': [run_experiment(image, output, 'small_then_large', ((2, 1), (8, 8))),
                             run_experiment(image, output, 'large_then_small', ((8, 8), (2, 1))),
                             run_experiment(image, output, 'small_then_large_synthetic_canary', ((2, 1), (8, 8)), True)]}
    (output / 'trace.json').write_text(json.dumps(plain(trace), indent=2) + '\n')
    (output / 'FINDINGS.md').write_text(findings(trace))
    trace['artifact_sha256'] = file_hashes(output)
    (output / 'trace.json').write_text(json.dumps(plain(trace), indent=2) + '\n')
    return trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=emulation.DEFAULT_IMAGE)
    parser.add_argument('--output', type=Path, help='new output directory (default: private temporary directory)')
    args = parser.parse_args()
    try:
        output = (args.output.expanduser().resolve() if args.output else
                  Path(tempfile.mkdtemp(prefix='ame-oeminfo-bmp-')))
        trace = analyse(args.image, output)
        print(f"Firmware SHA-256 verified: {trace['firmware']['sha256']}")
        for experiment in trace['experiments']:
            calls = experiment['centering_calls']
            print(f"{experiment['name']}: scratch reuse={calls[1]['scratch_reused']}, "
                  f"live pointer storage modified={any(c['pointer_storage']['live_pointer_storage_modified'] for c in calls)}")
        print(f'Results: {output}')
    except (OSError, ValueError, RuntimeError, emulation.unicorn.UcError) as error:
        parser.exit(2, f'OEMINFO BMP analysis: {error}\n')


if __name__ == '__main__':
    main()
