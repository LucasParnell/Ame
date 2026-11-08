#!/usr/bin/env python3
"""Replay the stock dts loader against a captured HSDT image in Unicorn.

The fastboot image is executed from dtb_init through its normal return. The
captured dts image supplies all storage reads. Module availability, the board
ID state and inert logging/timer/delay services are seeded explicitly and are
listed in the report. No device transport is used.

--preboot-dispatch instead enters the real sorted module dispatcher, models
one pre-dispatch boot-mode register page, and replaces only callbacks before
dts with zero-return stubs. dts and later callbacks execute from stock code.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

import unicorn
from unicorn import (UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_MEM_INVALID,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_PROT_ALL)
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                 UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                 UC_ARM64_REG_X19,
                                 UC_ARM64_REG_X22, UC_ARM64_REG_X24, UC_ARM64_REG_X29,
                                 UC_ARM64_REG_X30)


ROOT = Path(__file__).resolve().parents[3]
DEFAULT_FASTBOOT = ROOT / 'out/anne-device-private/fastboot-firmware-dump-20261101T213555Z/fastboot.img'
EMU_PATH = Path(__file__).with_name('emulate-fastboot.py')
DTS_PATH = Path(__file__).resolve().parents[1] / 'dts-el2-payload.py'
DTS_BASE = 0x50000000
READ_STUB = 0x70001000
DELAY_STUB = 0x70001004
INIT_STUB = 0x70001008
DTB_INIT = 0x6f74c
DTB_INIT_RETURN = 0x348
MAIN_MODULE_DISPATCH = 0x1111d8
MODULE_INIT_CALL = 0x1113ac
MODULE_INIT_RETURN = 0x1113b0
DTB_CAP = 0x200000
ARENA_B_BASE = 0x2e000000
ARENA_B_SIZE = 0x2000000
ARENA_A_BASE = 0x30000000
ARENA_A_SIZE = 0x4000000
OVERFLOW_START = 0x2e600000
REQUIRED_MODULES = (b'boardid', b'storage', b'fdt', b'partition', b'fault', b'secboot')
ALLOCATORS = (0x111680, 0x11173c)
CHECKPOINTS = {
    0x6f780: 'board-id-return',
    0x6eda0: 'inflate-length-ready',
    0x6edf0: 'dtb-verify-return',
    0x6f804: 'dtb-entry-return',
    DTB_INIT_RETURN: 'dtb-init-return',
}
OUTPUT_STORE_PCS = {
    0xcd2f4, 0xcd46c, 0xcd4ac, 0xcd4d0, 0xcd598,
    0xcd5b8, 0xcd5c4, 0xcd5cc, 0xcd5e0, 0xcd5f0,
    0xcd628, 0xcd670, 0xcd694, 0xcd6d0,
}


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError('could not load local helper %s' % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_address_range(value):
    try:
        address_text, size_text = value.split(':', 1)
        address, size = int(address_text, 0), int(size_text, 0)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError('expected ADDRESS:SIZE') from error
    if address < 0 or size <= 0 or address + size > 1 << 64:
        raise argparse.ArgumentTypeError('address range must be positive and fit in 64 bits')
    return address, size


def machine_words(image):
    branches = set()
    for (word,), offset in zip(struct.iter_unpack('<I', image[:len(image) & ~3]),
                                range(0, len(image) & ~3, 4)):
        instruction = word & 0xfffffc1f
        if instruction in (0xd63f0000, 0xd61f0000):  # BLR / BR
            branches.add(offset)
    return branches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fastboot', type=Path, default=DEFAULT_FASTBOOT)
    parser.add_argument('--dts-image', type=Path, required=True,
                        help='captured stock dts or locally rebuilt HSDT container')
    parser.add_argument('--partition-image', action='append', default=[], metavar='NAME=PATH',
                        help='additional captured partition image for normalboot storage reads (repeatable)')
    parser.add_argument('--entry', type=int, default=0,
                        help='HSDT entry index to inflate (default: handset key row 0)')
    parser.add_argument('--board-id', type=lambda x: int(x, 0), default=4007,
                        help='synthetic board ID; 4007 selects stock key 04 00 00 07')
    parser.add_argument('--count', type=int, default=10_000_000)
    parser.add_argument('--timeout-ms', type=int, default=60_000)
    parser.add_argument('--preboot-dispatch', action='store_true',
                        help='run the normal module dispatcher, synthesizing only callbacks before dts')
    parser.add_argument('--boot-mode', type=lambda x: int(x, 0),
                        help='seed the dispatcher mode field at 0x258c34 (synthetic)')
    parser.add_argument('--mode-before-module', metavar='MODULE',
                        help='apply --boot-mode immediately before this initializer instead of at loop start')
    parser.add_argument('--stop-after-module',
                        help='stop the offline replay after this module initializer returns')
    parser.add_argument('--capture-arena-a-before-dts', action='store_true',
                        help='write an Arena A snapshot immediately before the dts callback')
    parser.add_argument('--watch-pc', action='append', default=[], type=lambda x: int(x, 0),
                        metavar='ADDRESS',
                        help='record register state whenever this PC executes (repeatable)')
    parser.add_argument('--trace-module-calls', action='append', default=[], metavar='MODULE',
                        help='record indirect calls made inside this module initializer (repeatable)')
    parser.add_argument('--real-before-dts', action='append', default=[], metavar='MODULE',
                        help='in preboot mode, run this named pre-dts callback from stock code')
    parser.add_argument('--stub-after-dts', action='append', default=[], metavar='MODULE',
                        help='in preboot mode, replace this post-dts initializer with a zero-return stub')
    parser.add_argument('--stop-after-state-snapshot', action='store_true',
                        help='stop at the first zlib output write crossing the destination boundary')
    parser.add_argument('--no-overflow-write-trace', action='store_true',
                        help='omit per-write tracing for long full-output replays')
    parser.add_argument('--no-block-trace', action='store_true',
                        help='omit basic-block counting for long full-output replays')
    parser.add_argument('--ready-crypto-poll', action='store_true',
                        help=('model the crypto status bits at 0x7d624 and the engine-ready '
                              'wait at 0x7cb78 as ready'))
    parser.add_argument('--synthetic-secure-lcs', type=lambda x: int(x, 0),
                        choices=(5, 7), metavar='VALUE',
                        help=('offline only: return captured-firmware secure lifecycle '
                              'value 5 or 7 from FUN_00080160; this models the lifecycle '
                              'precondition but does not emulate efuse hardware'))
    parser.add_argument('--force-dtb-verify-success', action='store_true',
                        help=('offline flow-analysis only: skip the indirect crypto verifier at '
                              '0x6ea30 after the stock proof read and return success; this does '
                              'not validate the proof'))
    parser.add_argument('--advance-syscounter', action='store_true',
                        help=('advance the synthetic syscounter on reads at 0xad224; '
                              'lets firmware timer-delay loops complete offline'))
    parser.add_argument('--trace-inflate-branches', action='store_true',
                        help='record indirect branches inside the inflater without tracing every instruction')
    parser.add_argument('--trace-overread-dispatches', action='store_true',
                        help=('after dts, detect indirect calls whose target equals the first '
                              'field of a heap allocation overlapped by an oversized storage read'))
    parser.add_argument('--trace-overread-memory', action='store_true',
                        help=('record CPU reads and writes to bytes beyond any oversized '
                              'storage-read allocation, to identify data consumers'))
    parser.add_argument('--trace-memory-range', action='append', default=[],
                        type=parse_address_range, metavar='ADDRESS:SIZE',
                        help=('record ordered CPU and emulator-host memory accesses within '
                              'a narrow address range (repeatable)'))
    parser.add_argument('--fast-forward-inflate', action='store_true',
                        help=('offline shortcut: write the exact host-inflated output, advance the '
                              'observed zlib heap cursor, and resume at the real post-inflate code'))
    parser.add_argument('--fast-forward-kernel-gzip', action='store_true',
                        help=('offline shortcut: host-decompress the captured kernel gzip at '
                              '0xcfde0, preserve its output buffer and zlib allocator footprint, '
                              'and resume at the real caller'))
    parser.add_argument('--map-bfm-state-zero', action='store_true',
                        help='map the BFM fallback state page at 0x34a31000 as synthetic zero RAM')
    parser.add_argument('--map-boot-private-page-zero', action='store_true',
                        help='map the boot private-data page at 0x34900000 as synthetic zero RAM')
    parser.add_argument('--map-watchdog-register-page', action='store_true',
                        help='map the watchdog register page at 0xe8a06000 as synthetic zero MMIO')
    parser.add_argument('--map-ep-info-page-zero', action='store_true',
                        help='map the boot ep_info page at 0x35e80000 as synthetic zero RAM')
    parser.add_argument('--map-zero-page', action='append', default=[], type=lambda x: int(x, 0),
                        metavar='ADDRESS',
                        help='map one extra 4 KiB page as synthetic zero memory (repeatable)')
    parser.add_argument('--map-zero-range', action='append', default=[], metavar='ADDRESS:SIZE',
                        help='map an aligned range as synthetic zero memory (repeatable)')
    parser.add_argument('--stub-code', action='append', default=[], type=lambda x: int(x, 0),
                        metavar='ADDRESS',
                        help='replace one stock function entry with a synthetic zero-return stub')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    emu_mod = load_module(EMU_PATH, 'ame_emulate_fastboot')
    dts_mod = load_module(DTS_PATH, 'ame_dts_el2_payload')
    fastboot = args.fastboot.expanduser().read_bytes()
    dts_image = args.dts_image.expanduser().read_bytes()
    partition_images = {'dts': dts_image}
    for spec in args.partition_image:
        try:
            name, path_text = spec.split('=', 1)
        except ValueError:
            raise SystemExit('--partition-image expects NAME=PATH')
        if not name or name in partition_images:
            raise SystemExit('duplicate or empty partition name: ' + repr(name))
        path = Path(path_text).expanduser()
        partition_images[name] = path.read_bytes()
    if sha256(fastboot) != emu_mod.STOCK_SHA256:
        raise SystemExit('fastboot image hash does not match the pinned stock ANE-LX1 image')
    table = dts_mod.parse_table(dts_image)
    # An explicit --entry may select a non-default handset board row. Keep
    # the fixed stock key restriction in the DTS payload builder, while the
    # loader replay honors the selected table index exactly as the device does.
    selected = dts_mod.target_row(table, args.entry, require_proof=False,
                                  expected_key=None)
    inflated_reference = dts_mod.inflate_at(dts_image, selected)

    emulator = emu_mod.FastbootEmulator(fastboot)
    emulator.initialize_bss()
    heap = emulator.watch_heap()
    uc = emulator.uc

    def qword(address):
        return struct.unpack('<Q', uc.mem_read(address, 8))[0]

    def guest_string(address, limit=128):
        return bytes(uc.mem_read(address, limit)).split(b'\0', 1)[0]

    # Seed the same enabled-module state that exists when dtb_init runs in the
    # preboot module sequence. The function tables and their code remain stock.
    registry_start = qword(0x291a30)
    registry_end = qword(0x291868)
    if not (0 <= registry_start <= registry_end <= len(fastboot)):
        raise SystemExit('unexpected module registry bounds')
    modules = {}
    registry_modules = []
    for index in range((registry_end - registry_start) // 8):
        descriptor = qword(registry_start + index * 8)
        name = guest_string(qword(descriptor))
        priority = uc.mem_read(descriptor + 8, 1)[0]
        registry_modules.append({'name': name, 'descriptor': descriptor,
                                 'priority': priority,
                                 'entry': qword(descriptor + 0x10)})
        if name in REQUIRED_MODULES:
            modules[name] = descriptor
    missing = [name.decode() for name in REQUIRED_MODULES if name not in modules]
    if missing:
        raise SystemExit('required modules missing from stock registry: ' + ', '.join(missing))

    dispatch_stubbed_modules = []
    dispatch_stubbed_post_dts_modules = []
    if args.preboot_dispatch:
        sorted_modules = sorted(registry_modules, key=lambda item: item['priority'])
        dts_index = next((index for index, item in enumerate(sorted_modules)
                          if item['name'] == b'dts'), None)
        if dts_index is None:
            raise SystemExit('dts module missing from stock dispatcher registry')
        available_pre_dts = {item['name'].decode('ascii', 'replace')
                             for item in sorted_modules[:dts_index]}
        unknown_real_modules = sorted(set(args.real_before_dts) - available_pre_dts)
        if unknown_real_modules:
            raise SystemExit('not a pre-dts module: ' + ', '.join(unknown_real_modules))
        available_post_dts = {item['name'].decode('ascii', 'replace')
                              for item in sorted_modules[dts_index + 1:]}
        unknown_post_modules = sorted(set(args.stub_after_dts) - available_post_dts)
        if unknown_post_modules:
            raise SystemExit('not a post-dts module: ' + ', '.join(unknown_post_modules))
        for item in sorted_modules[:dts_index]:
            name = item['name'].decode('ascii', 'replace')
            if item['name'] == b'boardid' or name in args.real_before_dts:
                # This initializer publishes the board ID used by dts. Its
                # backing state is seeded above. Selected callbacks can also
                # be kept real while ranking allocation/call paths.
                continue
            uc.mem_write(item['descriptor'] + 0x10, struct.pack('<Q', INIT_STUB))
            dispatch_stubbed_modules.append({
                'name': item['name'].decode('ascii', 'replace'),
                'priority': item['priority'],
                'original_entry': hex(item['entry']),
                'synthetic_entry': hex(INIT_STUB),
            })
        for item in sorted_modules[dts_index + 1:]:
            name = item['name'].decode('ascii', 'replace')
            if name in args.stub_after_dts:
                uc.mem_write(item['descriptor'] + 0x10, struct.pack('<Q', INIT_STUB))
                dispatch_stubbed_post_dts_modules.append({
                    'name': name, 'priority': item['priority'],
                    'original_entry': hex(item['entry']),
                    'synthetic_entry': hex(INIT_STUB),
                })
    else:
        for descriptor in modules.values():
            uc.mem_write(descriptor + 0x0c, struct.pack('<I', 1))

    # The boardid accessor reads this post-platform-init field at +8.
    board_state = qword(0x291cc8)
    if not (0 <= board_state < len(fastboot) and board_state + 12 <= len(fastboot)):
        raise SystemExit('unexpected board-ID state pointer')
    uc.mem_write(board_state + 8, struct.pack('<I', args.board_id))

    # Replace only storage read with a bounded reader over the captured image.
    storage_api = qword(modules[b'storage'] + 0x18)
    fault_api = qword(modules[b'fault'] + 0x18)
    emulator.map(DTS_BASE, len(dts_image), 'captured HSDT input')
    uc.mem_write(DTS_BASE, dts_image)
    emulator.map(READ_STUB, 0x1000, 'synthetic storage/delay stubs', UC_PROT_ALL)
    emulator.map(0x07aff000, 0x1000, 'modeled SoC ID register page', UC_PROT_ALL)
    emulator.map(0xff000000, 0x100000, 'modeled crypto-engine MMIO aperture', UC_PROT_ALL)
    emulator.map(0xfff00000, 0x100000, 'modeled platform MMIO aperture', UC_PROT_ALL)
    if args.map_bfm_state_zero:
        emulator.map(0x34a31000, 0x1000, 'synthetic zero BFM fallback state', UC_PROT_ALL)
    if args.map_boot_private_page_zero:
        emulator.map(0x34900000, 0x1000, 'synthetic zero boot private-data page', UC_PROT_ALL)
    if args.map_watchdog_register_page:
        emulator.map(0xe8a06000, 0x1000, 'synthetic zero watchdog register page', UC_PROT_ALL)
    if args.map_ep_info_page_zero:
        emulator.map(0x35e80000, 0x1000, 'synthetic zero boot ep_info page', UC_PROT_ALL)
    if any(address % 0x1000 for address in args.map_zero_page):
        raise SystemExit('--map-zero-page addresses must be 4 KiB aligned')
    for address in args.map_zero_page:
        emulator.map(address, 0x1000, 'synthetic zero page at 0x%x' % address, UC_PROT_ALL)
    zero_ranges = []
    for spec in args.map_zero_range:
        try:
            address_text, size_text = spec.split(':', 1)
            address, size = int(address_text, 0), int(size_text, 0)
        except ValueError:
            raise SystemExit('--map-zero-range expects ADDRESS:SIZE')
        if address % 0x1000 or size <= 0 or size % 0x1000:
            raise SystemExit('--map-zero-range address and size must be 4 KiB aligned')
        emulator.map(address, size, 'synthetic zero range at 0x%x' % address, UC_PROT_ALL)
        zero_ranges.append((address, size))
    for address in args.stub_code:
        if not emulator.mapping(address) or not emulator.mapping(address + 7):
            raise SystemExit('--stub-code address must be in mapped fastboot memory')
        uc.mem_write(address, struct.pack('<II', 0xd2800000, 0xd65f03c0))
    uc.mem_write(READ_STUB, struct.pack('<II', 0xd65f03c0, 0xd65f03c0))
    uc.mem_write(INIT_STUB, struct.pack('<II', 0x52800000, 0xd65f03c0))
    uc.mem_write(storage_api, struct.pack('<Q', READ_STUB))
    uc.mem_write(fault_api + 8, struct.pack('<Q', DELAY_STUB))

    # Quiet console and monotonic-clock services; loader, inflater, FDT and
    # secboot code execute from the unchanged image.
    ret = struct.pack('<I', 0xd65f03c0)
    uc.mem_write(0x111438, ret)
    uc.mem_write(0xae78c, ret)
    uc.mem_write(0xaad20, struct.pack('<II', 0xd2800000, 0xd65f03c0))
    if args.preboot_dispatch:
        # This startup helper programs boot-mode MMIO at 0xe896c000 before the
        # module loop. The next startup helper initializes GIC registers and
        # changes SCR_EL3; timer startup reads the syscounter MMIO. Skip these
        # hardware-only routines before the loop.
        uc.mem_write(0xae51c, ret)
        uc.mem_write(0x1a498, struct.pack('<II', 0x52800000, 0xd65f03c0))
        uc.mem_write(0xad2e4, ret)
        uc.mem_write(0xad22c, ret)

    storage_reads = []
    checkpoints = {}
    overflow_writes = {'count': 0, 'start': None, 'end': None,
                       'pc_counts': {}, 'samples': []}
    overflow_state_snapshot = {'address': None, 'bytes': None, 'pc': None,
                               'registers': None}
    faults = []
    module_calls = []
    module_indirect_calls = []
    active_module_call = None
    boot_mode_seeds = []
    inflation_active = False
    inflater_return_pc = None
    payload_execution = []
    inflater_indirect_branches = []
    allocation_field_dispatches = {}
    oversized_storage_reads = []
    overread_dispatches = {}
    overread_overlapped_allocations = []
    overread_memory_ranges = []
    overread_memory_hooks = []
    overread_memory_read_count = 0
    overread_memory_write_count = 0
    overread_memory_tainted_read_count = 0
    overread_memory_tainted_write_count = 0
    overread_memory_tainted_bytes_cleared = 0
    overread_memory_host_tainted_write_count = 0
    overread_memory_host_tainted_bytes_cleared = 0
    overread_memory_reads = {}
    overread_memory_writes = {}
    overread_memory_host_writes = {}
    memory_trace_events = []
    memory_trace_truncated = False
    memory_trace_sequence = 0
    scanned_heap_allocations = 0
    dts_module_returned = False
    hardware_ready_polls = []
    synthetic_lcs_overrides = []
    synthetic_crypto_calls = []
    synthetic_kernel_inflations = []
    arena_a_before_dts = None
    syscounter_value = 0
    syscounter_reads = []
    watched_pc_hits = []
    stop = {'reason': None, 'value': None, 'blocks': 0}
    instruction_hooks = {}

    def trace_memory_access(kind, uc, address, size, value=None, source=None, data=None):
        nonlocal memory_trace_sequence, memory_trace_truncated
        overlaps = []
        for range_address, range_size in args.trace_memory_range:
            start = max(address, range_address)
            end = min(address + size, range_address + range_size)
            if start < end:
                overlaps.append((start, end))
        if not overlaps:
            return
        if len(memory_trace_events) >= 16384:
            memory_trace_truncated = True
            return
        memory_trace_sequence += 1
        event = {
            'sequence': memory_trace_sequence,
            'kind': kind,
            'address': hex(address),
            'size': size,
            'overlaps': [{'start': hex(start), 'end': hex(end)}
                         for start, end in overlaps],
            'pc': hex(uc.reg_read(UC_ARM64_REG_PC)),
            'module': active_module_call['name'] if active_module_call else None,
            'x0': hex(uc.reg_read(UC_ARM64_REG_X0)),
            'x1': hex(uc.reg_read(UC_ARM64_REG_X1)),
            'x30': hex(uc.reg_read(UC_ARM64_REG_X30)),
        }
        if source is not None:
            event['source'] = source
        if data is not None:
            payload = bytes(data)
            event['written_bytes'] = [
                {'start': hex(start),
                 'bytes': payload[start - address:end - address][:64].hex()}
                for start, end in overlaps]
        else:
            event['memory_bytes_before_access'] = []
            for start, end in overlaps:
                try:
                    sample_size = min(end - start, 64)
                    sample = bytes(uc.mem_read(start, sample_size)).hex()
                except unicorn.UcError:
                    sample = None
                event['memory_bytes_before_access'].append(
                    {'start': hex(start), 'bytes': sample})
        if value is not None:
            event['value'] = hex(value)
        memory_trace_events.append(event)

    def on_trace_memory_read(uc, _access, address, size, value, _):
        trace_memory_access('cpu-read', uc, address, size, value=value)

    def on_trace_memory_write(uc, _access, address, size, value, _):
        trace_memory_access('cpu-write', uc, address, size, value=value)

    def record_overread_memory_access(kind, uc, address, size, value):
        nonlocal overread_memory_read_count, overread_memory_write_count
        nonlocal overread_memory_tainted_read_count, overread_memory_tainted_write_count
        nonlocal overread_memory_tainted_bytes_cleared
        matched = [item['read_index'] for item in overread_memory_ranges
                   if address < item['end'] and address + size > item['start']]
        if not matched:
            return
        access_end = address + size
        tainted_by_read = {}
        for item in overread_memory_ranges:
            if item['read_index'] not in matched:
                continue
            tainted = 0
            for start, end in item['tainted_ranges']:
                tainted += max(0, min(access_end, end) - max(address, start))
            if tainted:
                tainted_by_read[item['read_index']] = tainted
        tainted_bytes = sum(tainted_by_read.values())
        if kind == 'read':
            overread_memory_read_count += 1
            if tainted_bytes:
                overread_memory_tainted_read_count += 1
            events = overread_memory_reads
        else:
            overread_memory_write_count += 1
            if tainted_bytes:
                overread_memory_tainted_write_count += 1
                overread_memory_tainted_bytes_cleared += tainted_bytes
            events = overread_memory_writes
        if kind == 'write' and tainted_bytes:
            # CPU stores invalidate storage-read provenance for only the bytes
            # they replace. Do this even after the bounded event log fills: the
            # provenance state is part of the analysis, not the event sample.
            for tracked in overread_memory_ranges:
                if tracked['read_index'] not in tainted_by_read:
                    continue
                updated = []
                for start, end in tracked['tainted_ranges']:
                    if access_end <= start or address >= end:
                        updated.append((start, end))
                        continue
                    if start < address:
                        updated.append((start, address))
                    if access_end < end:
                        updated.append((access_end, end))
                tracked['tainted_ranges'] = updated
        pc = uc.reg_read(UC_ARM64_REG_PC)
        sample_length = min(size, 0x20)
        try:
            sample = bytes(uc.mem_read(address, sample_length)).hex()
        except unicorn.UcError:
            sample = None
        module = active_module_call['name'] if active_module_call else None
        register_context = None
        if kind == 'read':
            register_context = (uc.reg_read(UC_ARM64_REG_X0),
                                uc.reg_read(UC_ARM64_REG_X30))
        key = (pc, address, size, module, tuple(matched), sample, tainted_bytes,
               tuple(sorted(tainted_by_read.items())), register_context)
        item = events.get(key)
        if item is None:
            if len(events) >= 4096:
                return
            else:
                item = {'pc': hex(pc), 'address': hex(address), 'size': size,
                        'module': module, 'overread_indices': matched,
                        'sample_hex': sample, 'tainted_bytes': tainted_bytes,
                        'tainted_bytes_by_read': tainted_by_read, 'count': 0}
                if register_context is not None:
                    item['x0'] = hex(register_context[0])
                    item['x30'] = hex(register_context[1])
                    item['caller_pc'] = hex((register_context[1] - 4) & 0xffffffffffffffff)
                events[key] = item
        if item is not None:
            item['count'] += 1

    def on_overread_read(uc, _access, address, size, value, _):
        if args.trace_overread_memory:
            record_overread_memory_access('read', uc, address, size, value)

    def on_overread_write(uc, _access, address, size, value, _):
        if args.trace_overread_memory:
            record_overread_memory_access('write', uc, address, size, value)

    def host_mem_write(uc, address, data, source='emulator'):
        """Write synthetic/emulated data and invalidate stale read provenance."""
        nonlocal overread_memory_host_tainted_write_count
        nonlocal overread_memory_host_tainted_bytes_cleared
        payload = bytes(data)
        if args.trace_memory_range and payload:
            trace_memory_access('host-write', uc, address, len(payload),
                                source=source, data=payload)
        if args.trace_overread_memory and payload:
            access_end = address + len(payload)
            cleared_by_read = {}
            for tracked in overread_memory_ranges:
                updated = []
                cleared = 0
                for start, end in tracked['tainted_ranges']:
                    if access_end <= start or address >= end:
                        updated.append((start, end))
                        continue
                    overlap_start = max(address, start)
                    overlap_end = min(access_end, end)
                    cleared += overlap_end - overlap_start
                    if start < overlap_start:
                        updated.append((start, overlap_start))
                    if overlap_end < end:
                        updated.append((overlap_end, end))
                if cleared:
                    tracked['tainted_ranges'] = updated
                    cleared_by_read[tracked['read_index']] = cleared
            if cleared_by_read:
                overread_memory_host_tainted_write_count += 1
                overread_memory_host_tainted_bytes_cleared += sum(
                    cleared_by_read.values())
                pc = uc.reg_read(UC_ARM64_REG_PC)
                module = active_module_call['name'] if active_module_call else None
                key = (pc, address, len(payload), source,
                       tuple(sorted(cleared_by_read)), tuple(sorted(cleared_by_read.items())))
                item = overread_memory_host_writes.get(key)
                if item is None and len(overread_memory_host_writes) < 4096:
                    item = {'pc': hex(pc), 'address': hex(address),
                            'size': len(payload), 'source': source,
                            'module': module,
                            'overread_indices': sorted(cleared_by_read),
                            'tainted_bytes_cleared_by_read': cleared_by_read,
                            'count': 0}
                    overread_memory_host_writes[key] = item
                if item is not None:
                    item['count'] += 1
        uc.mem_write(address, payload)

    def on_storage_stub(uc, address, size, _):
        if address == DELAY_STUB:
            return
        name = guest_string(uc.reg_read(UC_ARM64_REG_X0)).decode('ascii', 'replace')
        offset = uc.reg_read(UC_ARM64_REG_X1)
        destination = uc.reg_read(UC_ARM64_REG_X2)
        length = uc.reg_read(UC_ARM64_REG_X3)
        source = partition_images.get(name)
        valid = (source is not None and offset <= len(source)
                 and length <= len(source) - offset)
        if valid:
            host_mem_write(uc, destination, source[offset:offset + length],
                           source='storage-read:' + name)
            result = 0
        else:
            result = 0xffffffff
        allocation = heap.owner(destination)
        if (valid and allocation is not None and allocation.get('status') == 'returned'
                and destination >= allocation.get('address', 0)
                and length > allocation.get('requested_end', destination) - destination):
            read_index = len(oversized_storage_reads)
            oversized_storage_reads.append({
                'read_index': read_index, 'partition': name,
                'offset': offset, 'destination': destination,
                'length': length, 'allocation': allocation.get('id'),
                'allocation_start': allocation.get('address'),
                'allocation_requested_end': allocation.get('requested_end'),
                'overflow_start': allocation.get('requested_end'),
                'overflow_end': destination + length,
            })
            if args.trace_overread_memory:
                trace_start = max(allocation.get('requested_end'), ARENA_A_BASE)
                trace_end = min(destination + length, ARENA_A_BASE + ARENA_A_SIZE)
                if trace_start < trace_end:
                    overread_memory_ranges.append({'read_index': read_index,
                                                   'partition': name,
                                                   'partition_offset': offset,
                                                   'destination': destination,
                                                   'length': length,
                                                   'start': trace_start,
                                                   'end': trace_end,
                                                   'tainted_ranges': [(trace_start,
                                                                       trace_end)]})
                    overread_memory_hooks.append(uc.hook_add(
                        UC_HOOK_MEM_READ, on_overread_read,
                        begin=trace_start, end=trace_end - 1))
                    overread_memory_hooks.append(uc.hook_add(
                        UC_HOOK_MEM_WRITE, on_overread_write,
                        begin=trace_start, end=trace_end - 1))
        storage_reads.append({'partition': name, 'offset': offset,
                              'destination': destination, 'length': length,
                              'success': valid})
        uc.reg_write(UC_ARM64_REG_X0, result)

    def track_pc(uc, address, size, _):
        nonlocal active_module_call, inflation_active, inflater_return_pc, boot_mode_seeds
        nonlocal syscounter_value, arena_a_before_dts, dts_module_returned
        nonlocal scanned_heap_allocations
        if args.fast_forward_kernel_gzip and address == 0xcfde0:
            input_pointer = uc.reg_read(emu_mod.REGISTERS['x0'])
            input_length = uc.reg_read(emu_mod.REGISTERS['x1'])
            output_pointer = uc.reg_read(emu_mod.REGISTERS['x4'])
            output_capacity = uc.reg_read(emu_mod.REGISTERS['x5'])
            output_length_pointer = uc.reg_read(emu_mod.REGISTERS['x6'])
            return_pc = uc.reg_read(UC_ARM64_REG_X30)
            if not all((input_pointer, input_length, output_pointer,
                        output_capacity, return_pc)):
                stop['reason'] = 'fast-forward-kernel-gzip-invalid-arguments'
                uc.emu_stop()
                return
            try:
                compressed = bytes(uc.mem_read(input_pointer, input_length))
                decompressed = gzip.decompress(compressed)
            except (gzip.BadGzipFile, EOFError, OSError, unicorn.UcError) as error:
                stop['reason'] = 'fast-forward-kernel-gzip-failed'
                stop['error'] = str(error)
                uc.emu_stop()
                return
            if len(decompressed) > output_capacity:
                stop['reason'] = 'fast-forward-kernel-gzip-output-exceeds-capacity'
                stop['error'] = '%#x output bytes exceed %#x-byte capacity' % (
                    len(decompressed), output_capacity)
                uc.emu_stop()
                return
            try:
                host_mem_write(uc, output_pointer, decompressed,
                               source='kernel-gzip-output')
                if output_length_pointer:
                    host_mem_write(uc, output_length_pointer,
                                   struct.pack('<I', len(decompressed)),
                                   source='kernel-gzip-length')
            except unicorn.UcError as error:
                stop['reason'] = 'fast-forward-kernel-gzip-output-unmapped'
                stop['error'] = str(error)
                stop['registers'] = {
                    'x0': hex(input_pointer), 'x1': hex(input_length),
                    'x4': hex(output_pointer), 'x5': hex(output_capacity),
                    'x6': hex(output_length_pointer), 'x30': hex(return_pc),
                }
                uc.emu_stop()
                return

            # 0xcfde0 allocates the same two zlib objects as the DTS gzip
            # wrapper. Model their exact monotonic-arena footprint before
            # resuming at the stock caller.
            cursor_address = 0x2ad4c0
            cursor = struct.unpack('<Q', uc.mem_read(cursor_address, 8))[0]
            allocator_specs = ((0xcfe38, 0x60), (0xcfe4c, 0x2548))
            for call_site, requested_size in allocator_specs:
                allocation_address = cursor
                reserved_end = (allocation_address + requested_size + 0x3f) & ~0x3f
                allocation = {
                    'id': len(heap.allocations) + 1,
                    'allocator': 0x11173c,
                    'allocator_name': 'arena-b',
                    'arena': ARENA_B_BASE,
                    '_cursor_address': cursor_address,
                    'caller': call_site,
                    'synthetic_invocation': True,
                    'return_pc': None,
                    'sp': uc.reg_read(UC_ARM64_REG_SP),
                    'size': requested_size,
                    'cursor_before': cursor,
                    'status': 'returned',
                    'address': allocation_address,
                    'cursor_after': reserved_end,
                    'requested_end': allocation_address + requested_size,
                    'reserved_end': reserved_end,
                    'first_bytes_at_return': bytes(uc.mem_read(
                        allocation_address, min(requested_size, 64))).hex(),
                }
                heap.allocations.append(allocation)
                cursor = reserved_end
            host_mem_write(uc, cursor_address, struct.pack('<Q', cursor),
                           source='kernel-gzip-allocator-cursor')
            synthetic_kernel_inflations.append({
                'pc': hex(address), 'input_address': hex(input_pointer),
                'input_bytes': input_length, 'input_sha256': sha256(compressed),
                'output_address': hex(output_pointer),
                'output_capacity': output_capacity,
                'output_bytes': len(decompressed),
                'output_sha256': sha256(decompressed),
                'output_length_pointer': hex(output_length_pointer),
                'return_pc': hex(return_pc), 'return_value': 0,
                'modeled_allocator_cursor': hex(cursor),
            })
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, return_pc)
            return
        if args.fast_forward_inflate and address == 0xcfac0:
            # The input stream was already inflated by dts_mod.inflate_at().
            # Preserve the actual output bytes and the observed bump-allocator
            # footprint of zlib (0x60 + 0x2548 bytes rounded to 64-byte
            # alignment), then continue at the caller's first post-inflate
            # instruction. This intentionally skips zlib's instruction path;
            # all loader checks, dtb_init error handling, and later callbacks
            # still execute from the stock image.
            output = uc.reg_read(emu_mod.REGISTERS['x4'])
            length_pointer = uc.reg_read(emu_mod.REGISTERS['x5'])
            return_pc = uc.reg_read(UC_ARM64_REG_X30)
            if output == 0 or length_pointer == 0 or not return_pc:
                stop['reason'] = 'fast-forward-invalid-inflater-arguments'
                uc.emu_stop()
                return
            boundary = ARENA_A_BASE
            first_part = min(len(inflated_reference), boundary - output)
            if first_part:
                host_mem_write(uc, output, inflated_reference[:first_part],
                               source='dts-inflate-output')
            if len(inflated_reference) > first_part:
                host_mem_write(uc, boundary, inflated_reference[first_part:],
                               source='dts-inflate-output')
            host_mem_write(uc, length_pointer,
                           struct.pack('<I', len(inflated_reference)),
                           source='dts-inflate-length')
            first_alloc = {'id': len(heap.allocations) + 1, 'allocator': 0x11173c,
                           'allocator_name': 'arena-b', 'arena': ARENA_B_BASE,
                           '_cursor_address': 0x2ad4c0, 'caller': 0xcfb1c,
                           'synthetic_invocation': True, 'return_pc': None,
                           'size': 0x60, 'cursor_before': OVERFLOW_START,
                           'status': 'returned', 'address': OVERFLOW_START,
                           'cursor_after': OVERFLOW_START + 0x60,
                           'requested_end': OVERFLOW_START + 0x60,
                           'reserved_end': OVERFLOW_START + 0x60,
                           'first_bytes_at_return': ''}
            second_address = OVERFLOW_START + 0x60
            second_cursor = OVERFLOW_START + 0x25c0
            second_alloc = {'id': len(heap.allocations) + 2, 'allocator': 0x11173c,
                            'allocator_name': 'arena-b', 'arena': ARENA_B_BASE,
                            '_cursor_address': 0x2ad4c0, 'caller': 0xcfb30,
                            'synthetic_invocation': True, 'return_pc': None,
                            'size': 0x2548, 'cursor_before': OVERFLOW_START + 0x60,
                            'status': 'returned', 'address': second_address,
                            'cursor_after': second_cursor,
                            'requested_end': second_address + 0x2548,
                            'reserved_end': second_cursor,
                            'first_bytes_at_return': ''}
            heap.allocations.extend((first_alloc, second_alloc))
            host_mem_write(uc, 0x2ad4c0, struct.pack('<Q', second_cursor),
                           source='dts-inflate-allocator-cursor')
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, return_pc)
            inflation_active = False
            return
        if address == READ_STUB:
            on_storage_stub(uc, address, size, None)
        if args.advance_syscounter and address == 0xad224:
            syscounter_value = (syscounter_value + 0x1000) & 0xffffffff
            host_mem_write(uc, 0xfff08008, struct.pack('<I', syscounter_value),
                           source='syscounter-register')
            if len(syscounter_reads) < 256:
                syscounter_reads.append({'pc': hex(address),
                                         'counter': hex(syscounter_value)})
        if args.synthetic_secure_lcs is not None and address == 0x80160:
            lcs_pointer = uc.reg_read(UC_ARM64_REG_X0)
            return_pc = uc.reg_read(UC_ARM64_REG_X30)
            if lcs_pointer == 0 or return_pc == 0:
                stop['reason'] = 'synthetic-secure-lcs-invalid-arguments'
                uc.emu_stop()
                return
            host_mem_write(uc, lcs_pointer,
                           struct.pack('<I', args.synthetic_secure_lcs),
                           source='synthetic-lifecycle-output')
            synthetic_lcs_overrides.append({
                'pc': hex(address), 'return_pc': hex(return_pc),
                'out_pointer': hex(lcs_pointer), 'value': args.synthetic_secure_lcs,
            })
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, return_pc)
            return
        if args.ready_crypto_poll and address == 0x7d624:
            status_address = uc.reg_read(UC_ARM64_REG_X3)
            if emulator.mapping(status_address):
                before = struct.unpack('<I', uc.mem_read(status_address, 4))[0]
                host_mem_write(uc, status_address, struct.pack('<I', before | 1),
                               source='crypto-ready-register')
                hardware_ready_polls.append({'pc': hex(address),
                                             'status_address': hex(status_address),
                                             'before': before, 'after': before | 1})
        if args.ready_crypto_poll and address == 0x7cb78:
            # FUN_0007cb78 waits until (base + 0xe9c) has a nonzero low
            # nibble before copying the prepared crypto block. In the
            # observed secboot replay, base is 0xff011000. Model only that
            # engine-ready condition, and record the synthetic register write.
            status_address = uc.reg_read(UC_ARM64_REG_X0) + 0xe9c
            if emulator.mapping(status_address):
                before = struct.unpack('<I', uc.mem_read(status_address, 4))[0]
                host_mem_write(uc, status_address, struct.pack('<I', before | 1),
                               source='crypto-ready-register')
                hardware_ready_polls.append({'pc': hex(address),
                                             'status_address': hex(status_address),
                                             'before': before, 'after': before | 1})
        if args.ready_crypto_poll and address == 0x7d414:
            # FUN_0007d414 waits for crypto-base + 0xa00 to become nonzero;
            # its caller then requires status bit 2 before proceeding.
            status_address = uc.reg_read(UC_ARM64_REG_X0) + 0xa00
            if emulator.mapping(status_address):
                before = struct.unpack('<I', uc.mem_read(status_address, 4))[0]
                host_mem_write(uc, status_address, struct.pack('<I', before | 4),
                               source='crypto-ready-register')
                hardware_ready_polls.append({'pc': hex(address),
                                             'status_address': hex(status_address),
                                             'before': before, 'after': before | 4})
        if args.ready_crypto_poll and address == 0x7d4b8:
            # FUN_0007d428 waits for crypto-base + 0xba0 to equal 1 after
            # the engine command. This models that completion status only.
            status_address = uc.reg_read(UC_ARM64_REG_X19) + 0xba0
            if emulator.mapping(status_address):
                before = struct.unpack('<I', uc.mem_read(status_address, 4))[0]
                host_mem_write(uc, status_address, struct.pack('<I', 1),
                               source='crypto-ready-register')
                hardware_ready_polls.append({'pc': hex(address),
                                             'status_address': hex(status_address),
                                             'before': before, 'after': 1})
        if args.ready_crypto_poll and address == 0x7dd08:
            # FUN_0007dd08 waits for the engine-ready bit at base + 0xabc
            # before reading the completed block.
            status_address = uc.reg_read(UC_ARM64_REG_X0) + 0xabc
            if emulator.mapping(status_address):
                before = struct.unpack('<I', uc.mem_read(status_address, 4))[0]
                host_mem_write(uc, status_address, struct.pack('<I', before | 1),
                               source='crypto-ready-register')
                hardware_ready_polls.append({'pc': hex(address),
                                             'status_address': hex(status_address),
                                             'before': before, 'after': before | 1})
        if args.force_dtb_verify_success and address == 0x6ea30:
            # This is the BLR through the platform crypto verifier in
            # dtb_verify. Keep the stock proof read and buffer handling, but
            # model only the verifier's success return for offline flow
            # analysis. This deliberately does not establish signature
            # validity and is never used by device transport code.
            # A BLR writes LR only after this code hook returns. Resume at
            # the following instruction instead of using the stale x30.
            return_pc = address + 4
            if len(synthetic_crypto_calls) < 256:
                synthetic_crypto_calls.append({
                    'pc': hex(address), 'target': hex(uc.reg_read(UC_ARM64_REG_X3)),
                    'x0': hex(uc.reg_read(UC_ARM64_REG_X0)),
                    'x1': hex(uc.reg_read(UC_ARM64_REG_X1)),
                    'x2': hex(uc.reg_read(UC_ARM64_REG_X2)),
                    'link_register_before_blr': hex(uc.reg_read(UC_ARM64_REG_X30)),
                    'return_pc': hex(return_pc), 'result': 0,
                })
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, return_pc)
            return
        if address == 0xcfac0:
            inflation_active = True
            inflater_return_pc = uc.reg_read(UC_ARM64_REG_X30)
            if inflater_return_pc not in instruction_hooks:
                handle = uc.hook_add(UC_HOOK_CODE, track_pc,
                                    begin=inflater_return_pc, end=inflater_return_pc)
                instruction_hooks[inflater_return_pc] = handle
        elif inflation_active and address == inflater_return_pc:
            inflation_active = False
        if OVERFLOW_START <= address < ARENA_A_BASE + ARENA_A_SIZE:
            if len(payload_execution) < 256:
                payload_execution.append({
                    'pc': hex(address), 'x0': hex(uc.reg_read(UC_ARM64_REG_X0)),
                    'x1': hex(uc.reg_read(UC_ARM64_REG_X1)),
                    'x2': hex(uc.reg_read(UC_ARM64_REG_X2)),
                    'x30': hex(uc.reg_read(UC_ARM64_REG_X30)),
                })
        pending_allocator_return = any(item['return_pc'] == address for item in heap.pending)
        if (inflation_active and address not in ALLOCATORS
                and address not in CHECKPOINTS and not pending_allocator_return):
            if not (args.trace_inflate_branches and address in branch_addresses):
                return
            raw = bytes(uc.mem_read(address, 4))
            decoded = next(emulator.disassembler.disasm(raw, address, count=1), None)
            if decoded is not None and decoded.mnemonic in ('blr', 'br'):
                register = decoded.op_str.strip()
                register_id = emu_mod.REGISTERS.get(register)
                if register_id is not None:
                    target = uc.reg_read(register_id)
                    inflater_indirect_branches.append({
                        'pc': hex(address), 'register': register,
                        'target': hex(target),
                        'target_in_heap_arena': any(
                            arena <= target < arena + arena_size
                            for arena, arena_size in heap.arenas),
                        'x0': hex(uc.reg_read(UC_ARM64_REG_X0)),
                        'x1': hex(uc.reg_read(UC_ARM64_REG_X1)),
                        'x2': hex(uc.reg_read(UC_ARM64_REG_X2)),
                    })
            return
        if args.preboot_dispatch and address == MODULE_INIT_CALL:
            descriptor = uc.reg_read(UC_ARM64_REG_X22)
            name_pointer = qword(descriptor)
            name = guest_string(name_pointer).decode('ascii', 'replace')
            if (args.capture_arena_a_before_dts and name == 'dts'
                    and arena_a_before_dts is None):
                arena_a_before_dts = bytes(uc.mem_read(ARENA_A_BASE, ARENA_A_SIZE))
            active_module_call = {
                'name': name, 'descriptor': hex(descriptor),
                'entry': hex(uc.reg_read(UC_ARM64_REG_X1)),
                'priority': uc.mem_read(descriptor + 8, 1)[0],
                'status': 'entered',
            }
            if (args.boot_mode is not None and args.mode_before_module == name):
                uc.mem_write(0x258c34, struct.pack('<I', args.boot_mode))
                boot_mode_seeds.append({'address': '0x258c34', 'value': args.boot_mode,
                                        'pc': hex(address), 'before_module': name})
            module_calls.append(active_module_call)
        if args.preboot_dispatch and address == 0x111200:
            # main's startup clear runs after the initial test-state seeding.
            # Reapply the synthetic board ID after that clear and before init.
            uc.mem_write(board_state + 8, struct.pack('<I', args.board_id))
        if (args.preboot_dispatch and args.boot_mode is not None
                and args.mode_before_module is None and address == 0x1112cc
                and not boot_mode_seeds):
            # x20 points at the dispatcher parameter block (0x258c30) here;
            # seed its mode word after startup helpers and before the initial
            # mode snapshot is loaded into a callee-saved register.
            uc.mem_write(0x258c34, struct.pack('<I', args.boot_mode))
            boot_mode_seeds.append({'address': '0x258c34', 'value': args.boot_mode,
                                    'pc': hex(address), 'before_module': None})
        if address in args.watch_pc:
            watched_pc_hits.append({
                'pc': hex(address), 'x0': hex(uc.reg_read(UC_ARM64_REG_X0)),
                'x1': hex(uc.reg_read(UC_ARM64_REG_X1)),
                'x2': hex(uc.reg_read(UC_ARM64_REG_X2)),
                'x3': hex(uc.reg_read(UC_ARM64_REG_X3)),
                'x30': hex(uc.reg_read(UC_ARM64_REG_X30)),
                'module': active_module_call['name'] if active_module_call else None,
            })
        if args.preboot_dispatch and address == MODULE_INIT_RETURN and active_module_call:
            active_module_call['return_value'] = hex(uc.reg_read(UC_ARM64_REG_X0))
            active_module_call['status'] = ('returned' if uc.reg_read(UC_ARM64_REG_X0) == 0
                                            else 'failed')
            if active_module_call['name'] == args.stop_after_module:
                stop['reason'] = 'module-return:%s' % active_module_call['name']
                stop['value'] = uc.reg_read(UC_ARM64_REG_X0)
                uc.emu_stop()
            if active_module_call['name'] == 'dts':
                dts_module_returned = True
            active_module_call = None
        if address in CHECKPOINTS:
            item = {'label': CHECKPOINTS[address], 'pc': address,
                    'x0': uc.reg_read(UC_ARM64_REG_X0),
                    'x1': uc.reg_read(UC_ARM64_REG_X1),
                    'x2': uc.reg_read(UC_ARM64_REG_X2),
                    'x24': uc.reg_read(UC_ARM64_REG_X24),
                    'x29': uc.reg_read(UC_ARM64_REG_X29),
                    'sp': uc.reg_read(UC_ARM64_REG_SP)}
            if address == 0x6eda0:
                output = item['x24']
                item['inflated_length'] = struct.unpack('<I', uc.mem_read(item['x29'] + 0x7c, 4))[0]
                if output:
                    amount = min(item['inflated_length'], ARENA_B_SIZE - 0x400000)
                    item['output_prefix_sha256'] = sha256(bytes(uc.mem_read(output, min(amount, DTB_CAP))))
                    if item['inflated_length'] > DTB_CAP:
                        tail = bytes(uc.mem_read(OVERFLOW_START,
                                                 min(item['inflated_length'] - DTB_CAP,
                                                     ARENA_B_BASE + ARENA_B_SIZE - OVERFLOW_START)))
                        expected_tail = inflated_reference[DTB_CAP:item['inflated_length']]
                        item['overflow_tail_matches_input'] = tail == expected_tail
                        item['overflow_tail_bytes'] = len(tail)
                        item['overflow_tail_sha256'] = sha256(tail)
            elif address == 0x6edf0:
                item['verify_result'] = uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff
            elif address == 0x6f804:
                item['dtb_pointer'] = uc.reg_read(UC_ARM64_REG_X0)
                item['dtb_length'] = struct.unpack('<Q', uc.mem_read(item['x29'] + 0x58, 8))[0]
                if item['dtb_pointer'] and item['dtb_length'] <= DTB_CAP:
                    result_bytes = bytes(uc.mem_read(item['dtb_pointer'], item['dtb_length']))
                    item['dtb_sha256'] = sha256(result_bytes)
                    item['dtb_matches_inflated_entry'] = result_bytes == inflated_reference
            elif address == DTB_INIT_RETURN:
                stop['reason'] = 'dtb-init-return'
                stop['value'] = item['x0']
                uc.emu_stop()
            checkpoints[address] = item

        raw = bytes(uc.mem_read(address, 4))
        decoded = next(emulator.disassembler.disasm(raw, address, count=1), None)
        heap.on_code(address, decoded)
        if decoded is not None and decoded.mnemonic in ('blr', 'br'):
            register = decoded.op_str.strip()
            register_id = emu_mod.REGISTERS.get(register)
            if register_id is not None:
                target = uc.reg_read(register_id)
                arg0 = uc.reg_read(UC_ARM64_REG_X0)
                if (active_module_call is not None
                        and active_module_call['name'] in args.trace_module_calls):
                    module_indirect_calls.append({
                        'module': active_module_call['name'], 'pc': hex(address),
                        'register': register, 'target': hex(target),
                        'x0': hex(arg0), 'x1': hex(uc.reg_read(UC_ARM64_REG_X1)),
                        'x2': hex(uc.reg_read(UC_ARM64_REG_X2)),
                        'x3': hex(uc.reg_read(UC_ARM64_REG_X3)),
                        'x30': hex(uc.reg_read(UC_ARM64_REG_X30)),
                    })
                if args.trace_overread_dispatches and dts_module_returned:
                    arg1 = uc.reg_read(UC_ARM64_REG_X1)
                    arg2 = uc.reg_read(UC_ARM64_REG_X2)
                    arg3 = uc.reg_read(UC_ARM64_REG_X3)
                    if scanned_heap_allocations < len(heap.allocations):
                        for allocation in heap.allocations[scanned_heap_allocations:]:
                            base = allocation.get('address')
                            reserved_end = allocation.get('reserved_end')
                            if allocation.get('status') != 'returned' or not base or reserved_end is None:
                                continue
                            for read_index, read in enumerate(oversized_storage_reads):
                                read_start = read['overflow_start']
                                read_end = read['overflow_end']
                                if base < read_end and base + 8 > read_start and reserved_end > read_start:
                                    overread_overlapped_allocations.append((read_index, allocation))
                        scanned_heap_allocations = len(heap.allocations)
                    for read_index, allocation in overread_overlapped_allocations:
                        read = oversized_storage_reads[read_index]
                        read_start = read['overflow_start']
                        read_end = read['overflow_end']
                        base = allocation['address']
                        reserved_end = allocation['reserved_end']
                        first_field = heap.word(base)
                        route = None
                        table_address = None
                        if first_field == target:
                            route = 'overread-allocation-first-field'
                        elif first_field is not None and any(
                                arena <= first_field < arena + arena_size
                                for arena, arena_size in heap.arenas):
                            if heap.word(first_field) == target:
                                route = 'overread-allocation-first-field-ops-table'
                                table_address = first_field
                        if route is None:
                            continue
                        key = (read_index, allocation.get('id'), address, target, route)
                        item = overread_dispatches.get(key)
                        if item is None:
                            overlap_start = max(base, read_start)
                            overlap_end = min(reserved_end, read_end)
                            item = {
                                'read_index': read_index,
                                'read_partition': read['partition'],
                                'read_offset': read['offset'],
                                'read_overflow_start': read_start,
                                'read_overflow_end': read_end,
                                'allocation': allocation.get('id'),
                                'allocation_address': base,
                                'allocation_size': allocation.get('size'),
                                'allocation_caller': allocation.get('caller'),
                                'allocation_overlap_start': overlap_start,
                                'allocation_overlap_end': overlap_end,
                                'allocation_overlap_bytes': max(0, overlap_end - overlap_start),
                                'first_field_value': first_field,
                                'ops_table': table_address,
                                'route': route,
                                'branch_pc': address,
                                'branch_target': target,
                                'branch_register': register,
                                'x0': arg0,
                                'x0_allocation': (heap.owner(arg0) or {}).get('id'),
                                'x1': arg1, 'x2': arg2, 'x3': arg3,
                                'module': active_module_call['name'] if active_module_call else None,
                                'calls': 0,
                            }
                            overread_dispatches[key] = item
                        item['calls'] += 1
                # The dispatch shape under investigation forwards x0 as the
                # object pointer. Scan that object's allocation, then inspect
                # any one-level ops-table pointer found in its fields.
                owner = heap.owner(arg0)
                for allocation in ([owner] if owner is not None else []):
                    base = allocation.get('address')
                    allocation_size = allocation.get('size', 0)
                    if not base or allocation.get('status') != 'returned' or allocation_size < 8:
                        continue
                    scan_size = min(allocation_size, 0x100)
                    try:
                        raw_fields = bytes(uc.mem_read(base, scan_size))
                    except unicorn.UcError:
                        continue
                    for field_offset in range(0, len(raw_fields) - 7, 8):
                        value = struct.unpack_from('<Q', raw_fields, field_offset)[0]
                        route = None
                        table_address = None
                        if value == target:
                            route = 'allocation-field'
                        elif any(arena <= value < arena + arena_size
                                 for arena, arena_size in heap.arenas):
                            table_first = heap.word(value)
                            if table_first == target:
                                route = 'allocation-field-ops-table'
                                table_address = value
                        if route is None:
                            continue
                        key = (allocation['id'], field_offset, address, target, route)
                        item = allocation_field_dispatches.get(key)
                        if item is None:
                            item = {
                                'allocation': allocation['id'], 'address': base,
                                'allocation_size': allocation_size,
                                'allocation_caller': allocation.get('caller'),
                                'field_offset': field_offset,
                                'field_value': value, 'ops_table': table_address,
                                'allocation_bytes_at_dispatch': raw_fields[:allocation_size].hex(),
                                'branch_pc': address, 'branch_target': target,
                                'branch_register': register, 'x0': arg0,
                                'x0_is_allocation_base': arg0 == base,
                                'x0_allocation': (heap.owner(arg0) or {}).get('id'),
                                # This is an address relationship, not an
                                # allocation-order relationship.  The latter
                                # is filled in once the DTB output allocation
                                # is known below.
                                'address_at_or_after_overflow_start': (
                                    base >= OVERFLOW_START),
                                'allocated_after_dtb_output': None,
                                'calls': 0,
                            }
                            allocation_field_dispatches[key] = item
                        item['calls'] += 1
        if address in ALLOCATORS:
            return_pc = uc.reg_read(UC_ARM64_REG_X30)
            if return_pc not in instruction_hooks:
                handle = uc.hook_add(UC_HOOK_CODE, track_pc,
                                    begin=return_pc, end=return_pc)
                instruction_hooks[return_pc] = handle

    branch_addresses = machine_words(fastboot)
    interesting = branch_addresses | set(ALLOCATORS) | set(CHECKPOINTS) | {
        READ_STUB, DELAY_STUB, INIT_STUB, MAIN_MODULE_DISPATCH,
        MODULE_INIT_RETURN, 0x111200, 0x1112cc, 0x1113e8, 0xcfac0, 0x7d624,
    } | set(args.watch_pc)
    if args.ready_crypto_poll:
        interesting.update((0x7cb78, 0x7d414, 0x7d4b8, 0x7dd08))
    if args.synthetic_secure_lcs is not None:
        interesting.add(0x80160)
    if args.fast_forward_kernel_gzip:
        interesting.add(0xcfde0)
    if args.advance_syscounter:
        interesting.add(0xad224)
    for address in sorted(interesting):
        handle = uc.hook_add(UC_HOOK_CODE, track_pc, begin=address, end=address)
        instruction_hooks[address] = handle
    payload_execution_hook = uc.hook_add(
        UC_HOOK_CODE, track_pc, begin=OVERFLOW_START,
        end=ARENA_A_BASE + ARENA_A_SIZE - 1)

    def on_block(_uc, _address, _size, _):
        stop['blocks'] += 1

    def on_invalid(uc, access, address, size, value, _):
        faults.append({'access': access, 'address': address, 'size': size,
                       'value': value, 'pc': uc.reg_read(UC_ARM64_REG_PC),
                       'x0': uc.reg_read(UC_ARM64_REG_X0),
                       'x1': uc.reg_read(UC_ARM64_REG_X1),
                       'x2': uc.reg_read(UC_ARM64_REG_X2),
                       'x3': uc.reg_read(UC_ARM64_REG_X3),
                       'x29': uc.reg_read(UC_ARM64_REG_X29),
                       'x30': uc.reg_read(UC_ARM64_REG_X30)})
        stop['reason'] = 'memory-fault'
        uc.emu_stop()
        return False

    def on_overflow_write(uc, _access, address, size, value, _):
        if not inflation_active:
            return
        pc = uc.reg_read(UC_ARM64_REG_PC)
        is_output_store = pc in OUTPUT_STORE_PCS
        crosses_output_boundary = address <= OVERFLOW_START < address + size
        if (overflow_state_snapshot['bytes'] is None and is_output_store
                and crosses_output_boundary):
            scratch_end = max((item.get('cursor_after', OVERFLOW_START)
                               for item in heap.allocations
                               if item.get('allocator_name') == 'arena-b'
                               and item.get('address', 0) >= OVERFLOW_START),
                              default=OVERFLOW_START)
            scratch_size = max(0, scratch_end - OVERFLOW_START)
            if scratch_size:
                overflow_state_snapshot.update(
                    address=OVERFLOW_START, bytes=bytes(uc.mem_read(OVERFLOW_START, scratch_size)),
                    pc=pc,
                    registers={name: uc.reg_read(register)
                               for name, register in (
                                   ('x0', UC_ARM64_REG_X0),
                                   ('x1', UC_ARM64_REG_X1),
                                   ('x2', UC_ARM64_REG_X2),
                                   ('x3', UC_ARM64_REG_X3),
                                   ('x8', emu_mod.REGISTERS['x8']),
                                   ('x9', emu_mod.REGISTERS['x9']),
                                   ('x10', emu_mod.REGISTERS['x10']),
                                   ('x11', emu_mod.REGISTERS['x11']),
                                   ('x12', emu_mod.REGISTERS['x12']),
                                   ('x13', emu_mod.REGISTERS['x13']),
                                   ('x24', UC_ARM64_REG_X24),
                                   ('x29', UC_ARM64_REG_X29),
                                   ('x30', UC_ARM64_REG_X30),
                                   ('sp', UC_ARM64_REG_SP))})
                if args.stop_after_state_snapshot:
                    stop['reason'] = 'state-snapshot-captured'
                    uc.emu_stop()
        overflow_writes['count'] += 1
        overflow_writes['start'] = (address if overflow_writes['start'] is None
                                    else min(address, overflow_writes['start']))
        overflow_writes['end'] = max(address + size,
                                     overflow_writes['end'] or address + size)
        pc_key = hex(pc)
        overflow_writes['pc_counts'][pc_key] = overflow_writes['pc_counts'].get(pc_key, 0) + 1
        if len(overflow_writes['samples']) < 512:
            overflow_writes['samples'].append({
                'pc': pc, 'address': address,
                'size': size, 'value': value,
            })

    hooks = [payload_execution_hook, uc.hook_add(UC_HOOK_MEM_INVALID, on_invalid)]
    for range_address, range_size in args.trace_memory_range:
        hooks.append(uc.hook_add(UC_HOOK_MEM_READ, on_trace_memory_read,
                                 begin=range_address,
                                 end=range_address + range_size - 1))
        hooks.append(uc.hook_add(UC_HOOK_MEM_WRITE, on_trace_memory_write,
                                 begin=range_address,
                                 end=range_address + range_size - 1))
    if not args.no_block_trace:
        hooks.append(uc.hook_add(UC_HOOK_BLOCK, on_block))
    if not args.no_overflow_write_trace:
        hooks.append(uc.hook_add(UC_HOOK_MEM_WRITE, on_overflow_write,
                                 begin=OVERFLOW_START,
                                 end=ARENA_A_BASE + ARENA_A_SIZE - 1))
    if args.preboot_dispatch:
        entry = MAIN_MODULE_DISPATCH
    else:
        entry = DTB_INIT
        uc.reg_write(UC_ARM64_REG_X30, DTB_INIT_RETURN)
    try:
        uc.emu_start(entry, 0 if args.preboot_dispatch else DTB_INIT_RETURN,
                     timeout=args.timeout_ms * 1000, count=args.count)
    except unicorn.UcError as error:
        if stop['reason'] is None:
            stop['reason'] = 'emulator-error'
            stop['error'] = str(error)
    finally:
        if stop['reason'] is None:
            pc = uc.reg_read(UC_ARM64_REG_PC)
            if not args.preboot_dispatch and pc == DTB_INIT_RETURN:
                stop['reason'] = 'dtb-init-return'
                stop['value'] = uc.reg_read(UC_ARM64_REG_X0)
            elif uc.query(unicorn.UC_QUERY_TIMEOUT):
                stop['reason'] = 'timeout'
            else:
                stop['reason'] = 'instruction-limit'
        for handle in hooks:
            uc.hook_del(handle)
        for handle in overread_memory_hooks:
            uc.hook_del(handle)
        for handle in instruction_hooks.values():
            uc.hook_del(handle)

    final_pc = uc.reg_read(UC_ARM64_REG_PC)
    output_allocation = next((item for item in heap.allocations
                              if item.get('allocator_name') == 'arena-b'
                              and item.get('size') == DTB_CAP), None)
    if output_allocation is not None and output_allocation.get('address'):
        dtb_output_id = output_allocation.get('id')
        dtb_output_start = output_allocation['address']
        dtb_overflow_start = dtb_output_start + DTB_CAP
        dtb_overflow_end = max(dtb_overflow_start,
                               dtb_output_start + len(inflated_reference))
        allocation_by_id = {item.get('id'): item for item in heap.allocations}
        for candidate in allocation_field_dispatches.values():
            allocation_start = candidate['address']
            allocation_end = allocation_start + candidate['allocation_size']
            field_start = allocation_start + candidate['field_offset']
            field_end = field_start + 8
            candidate_id = candidate['allocation']
            if dtb_output_id is not None and candidate_id is not None:
                candidate['allocated_after_dtb_output'] = candidate_id > dtb_output_id
                candidate['allocated_before_dtb_output'] = candidate_id < dtb_output_id
                candidate['dtb_output_allocation_id'] = dtb_output_id
            else:
                candidate['allocated_after_dtb_output'] = None
                candidate['allocated_before_dtb_output'] = None
                candidate['dtb_output_allocation_id'] = dtb_output_id
            candidate['dtb_overflow_range'] = {
                'start': dtb_overflow_start,
                'end': dtb_overflow_end,
            }
            candidate['allocation_overlaps_dtb_overflow'] = (
                allocation_start < dtb_overflow_end
                and allocation_end > dtb_overflow_start)
            candidate['dispatch_field_overlaps_dtb_overflow'] = (
                field_start < dtb_overflow_end
                and field_end > dtb_overflow_start)
            allocation_record = allocation_by_id.get(candidate_id, {})
            first_bytes = bytes.fromhex(
                allocation_record.get('first_bytes_at_return', ''))
            candidate['allocation_first_bytes_at_return'] = first_bytes.hex()
            candidate['dispatch_field_bytes_at_allocation_return'] = (
                first_bytes[candidate['field_offset']:candidate['field_offset'] + 8].hex())
            overlapping_reads = []
            for read in storage_reads:
                read_start = read.get('destination', 0)
                read_end = read_start + read.get('length', 0)
                overlap_start = max(allocation_start, read_start)
                overlap_end = min(allocation_end, read_end)
                if overlap_start >= overlap_end:
                    continue
                field_overlap_start = max(field_start, read_start)
                field_overlap_end = min(field_end, read_end)
                overlapping_reads.append({
                    'partition': read.get('partition'), 'offset': read.get('offset'),
                    'destination': read_start, 'length': read.get('length'),
                    'success': read.get('success'),
                    'object_overlap_start': overlap_start,
                    'object_overlap_bytes': overlap_end - overlap_start,
                    'dispatch_field_overlap_bytes': max(0, field_overlap_end - field_overlap_start),
                })
            candidate['overlapping_storage_reads'] = overlapping_reads
    output_comparison = None
    if output_allocation and output_allocation.get('address'):
        output_address = output_allocation['address']
        comparable = min(len(inflated_reference),
                         ARENA_A_BASE + ARENA_A_SIZE - output_address)
        actual = bytes(uc.mem_read(output_address, comparable))
        expected = inflated_reference[:comparable]
        matched_prefix = 0
        for actual_byte, expected_byte in zip(actual, expected):
            if actual_byte != expected_byte:
                break
            matched_prefix += 1
        matched_suffix = 0
        for actual_byte, expected_byte in zip(reversed(actual), reversed(expected)):
            if actual_byte != expected_byte:
                break
            matched_suffix += 1
        tail_size = max(0, comparable - DTB_CAP)
        actual_tail = actual[DTB_CAP:DTB_CAP + tail_size]
        expected_tail = expected[DTB_CAP:DTB_CAP + tail_size]
        tail_suffix = 0
        for actual_byte, expected_byte in zip(reversed(actual_tail), reversed(expected_tail)):
            if actual_byte != expected_byte:
                break
            tail_suffix += 1
        output_comparison = {
            'address': output_address,
            'expected_bytes': len(inflated_reference),
            'comparable_bytes': comparable,
            'matched_prefix_bytes': matched_prefix,
            'matched_suffix_bytes': matched_suffix,
            'matches_full_input': actual == expected,
            'overflow_start': output_address + DTB_CAP,
            'overflow_bytes_present': len(actual_tail),
            'overflow_tail_matched_suffix_bytes': tail_suffix,
            'overflow_tail_matches_input': actual_tail == expected_tail,
            'actual_overflow_sha256': sha256(actual_tail),
            'expected_overflow_sha256': sha256(expected_tail),
        }

    expected_overflow = max(0, len(inflated_reference) - DTB_CAP)
    expected_overflow_end = OVERFLOW_START + expected_overflow if expected_overflow else None
    overlap_allocations_after_inflate = []
    actual_write_start = overflow_writes['start']
    actual_write_end = overflow_writes['end']
    if actual_write_start is not None and actual_write_end is not None:
        for allocation in heap.allocations:
            base = allocation.get('address')
            size = allocation.get('size', 0)
            if not base or size <= 0 or allocation.get('status') != 'returned':
                continue
            overlap_start = max(base, actual_write_start)
            overlap_end = min(base + size, actual_write_end)
            if overlap_start >= overlap_end:
                continue
            capture_end = min(overlap_end, overlap_start + 0x100)
            overlap_allocations_after_inflate.append({
                'allocation': allocation.get('id'),
                'allocator': allocation.get('allocator_name'),
                'address': base,
                'size': size,
                'caller': allocation.get('caller'),
                'actual_write_overlap': {'start': overlap_start, 'end': overlap_end,
                                         'bytes': overlap_end - overlap_start},
                'captured_bytes': capture_end - overlap_start,
                'bytes_after_inflate': bytes(uc.mem_read(
                    overlap_start, capture_end - overlap_start)).hex(),
            })
    remaining_overread_taint = [
        {'read_index': tracked['read_index'], 'start': start, 'end': end,
         'bytes': end - start}
        for tracked in overread_memory_ranges
        for start, end in tracked['tainted_ranges']
    ]
    remaining_overread_taint_bytes = sum(
        item['bytes'] for item in remaining_overread_taint)
    overread_sources = {item['read_index']: item
                        for item in oversized_storage_reads}
    tainted_reads_by_partition = {}
    for event in overread_memory_reads.values():
        for read_index, tainted_bytes in event['tainted_bytes_by_read'].items():
            source = overread_sources.get(read_index, {})
            partition = source.get('partition', 'unknown')
            totals = tainted_reads_by_partition.setdefault(partition, {
                'read_events': 0, 'unique_event_shapes': 0,
                'tainted_bytes_observed': 0,
            })
            totals['read_events'] += event['count']
            totals['unique_event_shapes'] += 1
            totals['tainted_bytes_observed'] += tainted_bytes * event['count']
    remaining_taint_by_partition = {}
    for item in remaining_overread_taint:
        source = overread_sources.get(item['read_index'], {})
        partition = source.get('partition', 'unknown')
        remaining_taint_by_partition[partition] = (
            remaining_taint_by_partition.get(partition, 0) + item['bytes'])
    report = {
        'schema': 1,
        'fastboot_sha256': sha256(fastboot),
        'dts_image': str(args.dts_image.expanduser().resolve()),
        'dts_sha256': sha256(dts_image),
        'entry': selected['index'], 'key': selected['key'],
        'board_id': args.board_id,
        'board_id_bytes': [args.board_id // 1000 % 10, args.board_id // 100 % 10,
                           args.board_id // 10 % 10, args.board_id % 10],
        'inflated_input_bytes': len(inflated_reference),
        'expected_overflow': {'start': OVERFLOW_START if expected_overflow else None,
                              'end': expected_overflow_end, 'bytes': expected_overflow},
        'enabled_modules': {name.decode(): hex(address) for name, address in modules.items()},
        'preboot_dispatch': {
            'enabled': args.preboot_dispatch,
            'entry': hex(entry),
            'real_pre_dts_callbacks': args.real_before_dts,
            'synthetic_pre_dts_callbacks': dispatch_stubbed_modules,
            'synthetic_post_dts_callbacks': dispatch_stubbed_post_dts_modules,
            'synthetic_boot_mode_seeds': boot_mode_seeds,
            'stop_after_module': args.stop_after_module,
            'traced_module_calls': args.trace_module_calls,
            'module_calls': module_calls,
            'module_indirect_calls': module_indirect_calls,
            'payload_execution': payload_execution,
            'allocation_field_dispatch_candidates': sorted(
                allocation_field_dispatches.values(),
                key=lambda item: (item.get('allocated_after_dtb_output') is not True,
                                  not item['x0_is_allocation_base'],
                                  item['allocation'], item['branch_pc'], item['field_offset'])),
            'oversized_storage_reads': oversized_storage_reads,
            'overread_dispatch_candidates': sorted(
                overread_dispatches.values(),
                key=lambda item: (item['allocation'], item['branch_pc'], item['route'])),
        },
        'synthetic_hardware_ready_polls': hardware_ready_polls,
        'synthetic_lcs_overrides': synthetic_lcs_overrides,
        'synthetic_crypto_calls': synthetic_crypto_calls,
        'synthetic_kernel_inflations': synthetic_kernel_inflations,
        'arena_a_before_dts_snapshot': (
            {'address': hex(ARENA_A_BASE), 'bytes': len(arena_a_before_dts),
             'sha256': sha256(arena_a_before_dts)}
            if arena_a_before_dts is not None else None),
        'synthetic_syscounter_reads': syscounter_reads,
        'inflater_indirect_branches': inflater_indirect_branches,
        'watched_pc_hits': watched_pc_hits,
        'overread_memory_accesses': {
            'read_count': overread_memory_read_count,
            'write_count': overread_memory_write_count,
            'tainted_read_count': overread_memory_tainted_read_count,
            'tainted_write_count': overread_memory_tainted_write_count,
            'tainted_bytes_cleared': overread_memory_tainted_bytes_cleared,
            'host_tainted_write_count': overread_memory_host_tainted_write_count,
            'host_tainted_bytes_cleared': overread_memory_host_tainted_bytes_cleared,
            'tainted_bytes_remaining': remaining_overread_taint_bytes,
            'tracked_sources': [
                {'read_index': item['read_index'], 'partition': item['partition'],
                 'partition_offset': item['partition_offset'],
                 'destination': hex(item['destination']),
                 'length': item['length'], 'start': hex(item['start']),
                 'end': hex(item['end'])}
                for item in overread_memory_ranges],
            'tainted_reads_by_partition': tainted_reads_by_partition,
            'remaining_tainted_bytes_by_partition': remaining_taint_by_partition,
            'remaining_tainted_ranges': remaining_overread_taint[:128],
            'remaining_tainted_ranges_truncated': len(remaining_overread_taint) > 128,
            'provenance_note': (
                'CPU stores and emulator-host writes clear byte provenance from the '
                'oversized storage read. Host writes are listed separately. '
                'tainted_read_count counts CPU reads that still observe bytes from that '
                'read; register and arithmetic taint are not propagated.'),
            'reads': list(overread_memory_reads.values()),
            'writes': list(overread_memory_writes.values()),
            'host_writes': list(overread_memory_host_writes.values()),
            'truncated': (overread_memory_read_count > sum(
                item['count'] for item in overread_memory_reads.values())
                or overread_memory_write_count > sum(
                    item['count'] for item in overread_memory_writes.values())),
        },
        'memory_trace': {
            'ranges': [{'address': hex(address), 'size': size}
                       for address, size in args.trace_memory_range],
            'events': memory_trace_events,
            'truncated': memory_trace_truncated,
        },
        'payload_execution': payload_execution,
        'synthetic_services': {
            'storage_read': 'bounded reads from supplied local partition captures',
            'partition_images': {name: {'bytes': len(data), 'sha256': sha256(data)}
                                 for name, data in sorted(partition_images.items())},
            'inflater': ('host-inflated exact output with emulated zlib allocator footprint'
                         if args.fast_forward_inflate else 'stock firmware implementation'),
            'kernel_gzip': ('Python gzip output with emulated zlib allocator footprint at 0xcfde0'
                            if args.fast_forward_kernel_gzip else
                            'stock firmware implementation'),
            'dtb_crypto_verifier': (
                'forced success at 0x6ea30 after stock proof read; proof validity not checked'
                if args.force_dtb_verify_success else 'stock indirect verifier call'),
            'lifecycle': (('synthetic secure lifecycle value %d at 0x80160; efuse hardware '
                           'is not emulated' % args.synthetic_secure_lcs)
                          if args.synthetic_secure_lcs is not None else
                          'stock lifecycle getter'),
            'crypto_ready': ('synthetic crypto status at 0x7d624, 0x7cb78, 0x7d414, '
                             '0x7d4b8 and 0x7dd08'
                             if args.ready_crypto_poll else 'stock crypto status'),
            'board_id': 'seeded to the requested HSDT key',
            'delay': 'return immediately',
            'logs': 'no-op',
            'timer': ('syscounter advances by 0x1000 per read at 0xad224'
                      if args.advance_syscounter else 'zero-valued'),
            'patched_addresses': ['0x111438', '0xae78c', '0xaad20',
                                  hex(storage_api), hex(fault_api + 8)]
            + (['0xae51c', '0x1a498', '0xad2e4', '0xad22c'] if args.preboot_dispatch else []),
            'modeled_register_pages': ['0x07aff000..0x07afffff',
                                       '0xff000000..0xff0fffff',
                                       '0xfff00000..0xfffFFFFf']
            + (['0x34a31000..0x34a31fff (synthetic zero BFM state)']
               if args.map_bfm_state_zero else []),
            'modeled_ram_pages': (['0x34900000..0x34900fff (synthetic zero boot private data)']
                                  if args.map_boot_private_page_zero else []),
            'modeled_mmio_pages': (['0xe8a06000..0xe8a06fff (synthetic zero watchdog registers)']
                                   if args.map_watchdog_register_page else []),
            'modeled_boot_pages': (['0x35e80000..0x35e80fff (synthetic zero ep_info)']
                                   if args.map_ep_info_page_zero else []),
            'extra_zero_pages': ['0x%x..0x%x' % (address, address + 0xfff)
                                 for address in args.map_zero_page],
            'extra_zero_ranges': ['0x%x..0x%x' % (address, address + size - 1)
                                  for address, size in zero_ranges],
            'stubbed_code_entries': [hex(address) for address in args.stub_code],
        },
        'storage_reads': storage_reads,
        'checkpoints': {hex(address): value for address, value in checkpoints.items()},
        'output_comparison_after_execution': output_comparison,
        'overflow_write_trace_enabled': not args.no_overflow_write_trace,
        'basic_block_trace_enabled': not args.no_block_trace,
        'inflater_state_before_first_output_overrun': {
            'address': overflow_state_snapshot['address'],
            'bytes': len(overflow_state_snapshot['bytes'] or b''),
            'sha256': sha256(overflow_state_snapshot['bytes'] or b''),
            'pc': (hex(overflow_state_snapshot['pc'])
                   if overflow_state_snapshot['pc'] is not None else None),
            'registers': ({name: hex(value) for name, value
                           in overflow_state_snapshot['registers'].items()}
                          if overflow_state_snapshot['registers'] is not None else None),
        },
        'overflow_writes': overflow_writes,
        'overlap_allocations_after_inflate': overlap_allocations_after_inflate,
        'heap': heap.report(),
        'execution': {'stop_reason': stop['reason'], 'return_value': stop['value'],
                      'final_pc': hex(final_pc),
                      'basic_blocks': stop['blocks'], 'faults': faults,
                      'error': stop.get('error'),
                      'stop_registers': stop.get('registers')},
        'limits': {'instructions': args.count, 'timeout_ms': args.timeout_ms},
        'limitations': [
            ('Pre-dts module callbacks are replaced with zero-return stubs; their hardware side effects '
             'and state initialization are not reproduced.' if args.preboot_dispatch else
             'Module registry flags are set to the ready state expected at dtb_init; module initializers are not replayed.'),
            'Storage reads use supplied local partition images; this is not physical eMMC behavior.',
            ('The BFM fallback state page at 0x34a31000 is synthetic zero RAM.'
             if args.map_bfm_state_zero else
             'The BFM fallback state at 0x34a31000 is not mapped unless explicitly requested.'),
            ('The boot private-data page at 0x34900000 is synthetic zero RAM.'
             if args.map_boot_private_page_zero else
             'The boot private-data page at 0x34900000 is not mapped unless explicitly requested.'),
            ('The watchdog register page at 0xe8a06000 is synthetic zero MMIO.'
             if args.map_watchdog_register_page else
             'The watchdog register page at 0xe8a06000 is not mapped unless explicitly requested.'),
            ('The ep_info page at 0x35e80000 is synthetic zero RAM.'
             if args.map_ep_info_page_zero else
             'The ep_info page at 0x35e80000 is not mapped unless explicitly requested.'),
            'Extra --map-zero-page mappings are synthetic memory and do not model hardware behavior.',
            'Extra --map-zero-range mappings are synthetic memory and do not model hardware behavior.',
            ('The functions listed in --stub-code return synthetic zero; their hardware behavior is skipped.'
             if args.stub_code else
             'No stock function entries are replaced by synthetic stubs.'),
            'Console, timer and delay services are inert; they do not participate in parsing or verification.',
            ('Fast-forward mode skips execution inside 0xcfac0 and substitutes the exact Python-inflated output; '
             'post-inflate checks and later firmware callbacks remain stock code.'
             if args.fast_forward_inflate else
             'Inflate executes from the stock firmware implementation.'),
            ('Kernel-gzip fast-forward skips stock execution inside 0xcfde0 after Python validates the gzip '
             'member and writes the exact decompressed bytes; the caller and later firmware code remain stock.'
             if args.fast_forward_kernel_gzip else
             'Kernel gzip decompression executes from the stock firmware implementation.'),
            ('The indirect DTB crypto verifier at 0x6ea30 is forced to return success; this replay '
             'does not validate the stock proof.' if args.force_dtb_verify_success else
             'The stock indirect DTB crypto verifier executes without a synthetic result.'),
            'Only writes after the 2 MiB output buffer during the zlib call are recorded; the watched range includes arenas B and A.',
            'A successful control replay does not establish that an overflow reaches the later handoff dispatch.',
        ],
    }
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if arena_a_before_dts is not None:
        (output / 'arena-a-before-dts.bin').write_bytes(arena_a_before_dts)
    if overflow_state_snapshot['bytes'] is not None:
        (output / 'inflater-state-before-overflow.bin').write_bytes(
            overflow_state_snapshot['bytes'])
    emu_mod.save_allocations(output, heap.allocations)
    print(json.dumps({
        'stop_reason': stop['reason'], 'return_value': stop['value'],
        'basic_blocks': stop['blocks'], 'storage_reads': len(storage_reads),
        'allocations': len(heap.allocations),
        'overflow_writes': overflow_writes['count'],
        'verify_result': checkpoints.get(0x6edf0, {}).get('verify_result'),
        'dtb_pointer': checkpoints.get(0x6f804, {}).get('dtb_pointer'),
        'report': str(output / 'report.json'),
    }, indent=2))
    return 0 if stop['reason'] in ('dtb-init-return', 'state-snapshot-captured') else 1


if __name__ == '__main__':
    raise SystemExit(main())
