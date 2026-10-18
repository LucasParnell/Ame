#!/usr/bin/env python3
"""Analyse a raw AArch64 fastboot image with bounded, offline Unicorn execution."""

import argparse
from collections import Counter, deque
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

try:
    import capstone
    import unicorn
    from unicorn import arm64_const as arm64
except ImportError as error:
    raise SystemExit("Install analysis dependencies with .venv/bin/python -m pip install "
                     "-r scripts/anne/requirements-unicorn.txt") from error


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_IMAGE = ROOT / 'out/anne-device-private/el2-firmware-inspection/fastboot.img'
STOCK_SHA256 = '16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52'
PAGE = 0x1000
SCRATCH = 0x70000000
STACK = 0x7ff00000
STACK_SIZE = 0x10000
RETURN = 0x7ffe0000
REGISTERS = {f'x{i}': getattr(arm64, f'UC_ARM64_REG_X{i}') for i in range(31)}
REGISTERS.update(sp=arm64.UC_ARM64_REG_SP, pc=arm64.UC_ARM64_REG_PC,
                 nzcv=arm64.UC_ARM64_REG_NZCV)
TRAPS = {'smc', 'hvc', 'svc', 'brk', 'hlt', 'eret', 'wfi', 'wfe'}
ALLOCATOR = 0x111680
HEAP_CURSOR = 0x2ad498
HEAP_TOP = 0x2ad490
HEAP_BASE = 0x30000000
HEAP_SIZE = 0x4000000
BMP_CENTER = 0x4ebec
BMP_SCRATCH = 0x293608
LOGO_CHECKPOINTS = {BMP_CENTER: 'centering-entry', 0x4ecdc: 'before-clear',
                    0x4f028: 'after-centering'}
DISPLAY_LOGO = 0x4fac8
ACCESS_NAMES = {getattr(unicorn, f'UC_MEM_{name}'): name.lower().replace('_', '-')
                for name in ('READ_UNMAPPED', 'WRITE_UNMAPPED', 'FETCH_UNMAPPED',
                             'READ_PROT', 'WRITE_PROT', 'FETCH_PROT')}


class ExecutionMonitor:
    """Bounded per-run coverage, call history, and explicit memory watches."""

    def __init__(self, emulator, watches=(), stream=None, limit=20000):
        if not 1 <= limit <= 100000:
            raise ValueError('diagnostic limit must be 1..100000')
        self.emulator = emulator
        self.watches = list(watches)
        for address, size in self.watches:
            if address < 0 or size <= 0 or address + size > 1 << 64:
                raise ValueError('invalid watch range')
        self.stream, self.limit = stream, limit
        self.pcs, self.blocks, self.edges = Counter(), Counter(), Counter()
        self.truncated = set()
        self.previous_block = None
        self.pending_block = None
        self.block_count = 0
        self.events = deque(maxlen=min(limit, 256))
        self.event_count = 0
        self.calls = 0
        self.returns = 0
        self.step = 0

    def hit(self, counter, key, kind):
        if key in counter or len(counter) < self.limit:
            counter[key] += 1
        else:
            self.truncated.add(kind)

    def emit(self, kind, **fields):
        self.event_count += 1
        event = {'sequence': self.event_count, 'step': self.step, 'kind': kind, **fields}
        self.events.append(event)
        if self.stream is not None:
            self.stream.write(json.dumps(event) + '\n')

    def on_block(self, uc, address, size, _):
        # Unicorn can announce a block before enforcing the instruction limit.
        self.pending_block = (address, size)

    def on_code(self, address, decoded):
        self.step += 1
        if self.pending_block is not None:
            block_address, size = self.pending_block
            self.block_count += 1
            self.hit(self.blocks, (block_address, size), 'blocks')
            if self.previous_block is not None:
                self.hit(self.edges, (self.previous_block, block_address), 'edges')
            self.previous_block = block_address
            self.pending_block = None
        self.hit(self.pcs, address, 'instructions')
        if decoded is None:
            return
        if decoded.mnemonic in ('bl', 'blr', 'ret'):
            target = None
            if decoded.mnemonic == 'bl':
                target = int(decoded.op_str.lstrip('#'), 0)
            else:
                register = decoded.op_str.strip() or 'x30'
                if register in REGISTERS:
                    target = self.emulator.uc.reg_read(REGISTERS[register])
            if decoded.mnemonic == 'ret':
                self.returns += 1
            else:
                self.calls += 1
            self.emit('return' if decoded.mnemonic == 'ret' else 'call', pc=hex(address),
                      target=hex(target) if target is not None else None,
                      indirect=decoded.mnemonic != 'bl',
                      sp=hex(self.emulator.uc.reg_read(REGISTERS['sp'])))

    def on_memory(self, uc, access, address, size, value, _):
        matching = [hex(lo) + ':' + hex(length) for lo, length in self.watches
                    if address < lo + length and address + size > lo]
        if not matching:
            return
        # Unicorn reports writes before they commit; never label old bytes as the result.
        try:
            before = bytes(uc.mem_read(address, min(size, 64))).hex()
        except unicorn.UcError:
            before = None
        write = access == unicorn.UC_MEM_WRITE
        self.emit('memory-write' if write else 'memory-read',
                  pc=hex(uc.reg_read(REGISTERS['pc'])), address=hex(address), size=size,
                  watches=matching, bytes_before=before, bytes_truncated=size > 64,
                  **({'value': hex(value & ((1 << (min(size, 8) * 8)) - 1)),
                      'value_truncated': size > 8} if write else {}))

    def report(self):
        return {'coverage': {
                    'instructions': [{'pc': hex(pc), 'hits': hits} for pc, hits in sorted(self.pcs.items())],
                    'blocks': [{'pc': hex(pc), 'size': size, 'hits': hits}
                               for (pc, size), hits in sorted(self.blocks.items())],
                    'edges': [{'from': hex(source), 'to': hex(target), 'hits': hits}
                              for (source, target), hits in sorted(self.edges.items())],
                    'blocks_observed': self.block_count, 'entry_limit': self.limit,
                    'truncated': sorted(self.truncated)},
                'calls_observed': self.calls, 'returns_observed': self.returns,
                'watches': [{'address': hex(a), 'size': s} for a, s in self.watches],
                'event_count': self.event_count, 'recent_events': list(self.events),
                'events_truncated': self.event_count > len(self.events)}


class HeapMonitor:
    """Observe real allocator calls; never replace the allocator or its results."""

    def __init__(self, emulator, arena, allocator=ALLOCATOR, cursor=HEAP_CURSOR,
                 top=HEAP_TOP, checkpoints=(), stop_on_oob=False, event_limit=20000):
        self.emulator = emulator
        self.uc = emulator.uc
        self.arena = arena
        self.allocator, self.cursor, self.top = allocator, cursor, top
        self.checkpoints = set(checkpoints)
        self.stop_on_oob = stop_on_oob
        self.event_limit = event_limit
        self.allocations = []
        self.pending = []
        self.events = []
        self.event_count = 0
        self.violation_count = 0
        self.pointer_slots = {}
        self.pointer_provenance = {}
        self.stream = None
        self.stop_requested = False
        self.checkpoint_snapshots = []
        self.checkpoint_count = 0
        self.instruction_step = 0

    def snapshot(self):
        allocations = [dict(a) for a in self.allocations]
        pointer_slots = [{'address': a, 'value': v,
                          'allocation': (self.owner(a) or {}).get('id'),
                          'target_allocation': (self.owner(v) or {}).get('id'),
                          'provenance': self.pointer_provenance.get(a, 'unknown')}
                         for a, v in sorted(self.pointer_slots.items())]
        cursor = self.word(self.cursor)
        return {'cursor': cursor, 'cursor_initialized': cursor is not None and
                self.arena[0] <= cursor <= sum(self.arena), 'top': self.word(self.top),
                'scratch_pointer': self.word(BMP_SCRATCH), 'allocations': allocations,
                'allocation_count': len(allocations), 'pending_count': len(self.pending),
                'pointer_slots': pointer_slots,
                'pointer_bearing_allocations': sorted({s['allocation'] for s in pointer_slots
                                                      if s['allocation'] is not None}),
                'violation_count': self.violation_count}

    def checkpoint(self, address):
        snapshot = {'pc': address, 'label': LOGO_CHECKPOINTS.get(address, 'checkpoint'),
                    'registers': {name: hex(self.uc.reg_read(reg)) for name, reg in REGISTERS.items()},
                    **self.snapshot()}
        if address == 0x4ecdc:
            destination = self.uc.reg_read(REGISTERS['x0'])
            length = self.uc.reg_read(REGISTERS['x3'])
            snapshot['clear'] = {'destination': destination, 'capacity': self.uc.reg_read(REGISTERS['x1']),
                                 'value': self.uc.reg_read(REGISTERS['x2']) & 0xff, 'size': length}
            try:
                snapshot['clear']['bytes_before'] = bytes(self.uc.mem_read(destination, min(length, 64))).hex()
            except unicorn.UcError as error:
                snapshot['clear']['read_error'] = str(error)
        self.checkpoint_count += 1
        if len(self.checkpoint_snapshots) < 64:
            self.checkpoint_snapshots.append(snapshot)
        self.emit('checkpoint', **snapshot)
        return snapshot

    def word(self, address):
        try:
            return struct.unpack('<Q', self.uc.mem_read(address, 8))[0]
        except unicorn.UcError:
            return None

    def emit(self, kind, **fields):
        event = {'sequence': self.event_count + 1, 'step': self.instruction_step, 'kind': kind, **fields}
        self.event_count += 1
        if len(self.events) < self.event_limit:
            self.events.append(event)
        if self.stream is not None:
            self.stream.write(json.dumps(event) + '\n')

    def owner(self, address):
        return next((a for a in reversed(self.allocations)
                     if a.get('address') and a['address'] <= address < a['reserved_end']), None)

    def describe_range(self, address, size):
        end = address + size
        allocation = self.owner(address)
        if allocation is None:
            allocation = next((a for a in reversed(self.allocations) if a.get('address')
                               and address < a['reserved_end'] and end > a['address']), None)
        mapping = self.emulator.mapping(address)
        return {'start': address, 'end': end, 'size': size,
                'mapping': mapping['name'] if mapping else None,
                'allocation': allocation['id'] if allocation else None,
                'allocation_start': allocation.get('address') if allocation else None,
                'requested_end': allocation.get('requested_end') if allocation else None,
                'reserved_end': allocation.get('reserved_end') if allocation else None,
                'within_requested': bool(allocation and address >= allocation['address']
                                         and end <= allocation['requested_end']),
                'within_reservation': bool(allocation and address >= allocation['address']
                                           and end <= allocation['reserved_end']),
                'touches_heap_arena': address < sum(self.arena) and end > self.arena[0]}

    def memory_operation(self, address):
        if address == 0x4ec98:
            kind, source = 'clear-scratch-buffer', None
        elif address == 0x4ecb0:
            kind, source = 'copy-decoded-pixels-to-scratch', self.uc.reg_read(REGISTERS['x2'])
        elif address == 0x4ecdc:
            kind, source = 'clear-display-destination', None
        elif address == 0x4ed34:
            kind, source = 'copy-centered-row', self.uc.reg_read(REGISTERS['x2'])
        else:
            return None
        destination = self.uc.reg_read(REGISTERS['x0'])
        length = self.uc.reg_read(REGISTERS['x3'])
        operation = {'callsite': address, 'operation': kind,
                     'capacity_argument': self.uc.reg_read(REGISTERS['x1']),
                     'destination': self.describe_range(destination, length)}
        if source is not None:
            operation['source'] = self.describe_range(source, length)
        self.emit('memory-operation', **operation)
        return operation

    def check_range(self, pc, address, size, operation, expected=None):
        if size <= 0:
            return
        lo, length = self.arena
        allocation = expected or self.owner(address)
        touches = address < lo + length and address + size > lo
        if allocation is None and not touches:
            return
        if allocation is None or address < allocation['address'] or address + size > allocation['requested_end']:
            self.violation_count += 1
            self.emit('bounds-violation', pc=pc, address=address, size=size,
                      operation=operation, allocation=allocation['id'] if allocation else None,
                      requested_end=allocation['requested_end'] if allocation else None,
                      reserved_end=allocation['reserved_end'] if allocation else None,
                      crosses_reservation=allocation is None or address < allocation['address']
                      or address + size > allocation['reserved_end'])
            if self.stop_on_oob:
                self.stop_requested = True
                self.uc.emu_stop()

    def complete_return(self, address):
        sp = self.uc.reg_read(REGISTERS['sp'])
        if self.pending and address == self.pending[-1]['return_pc'] and sp == self.pending[-1]['sp']:
            allocation = self.pending.pop()
            allocation['address'] = self.uc.reg_read(REGISTERS['x0'])
            allocation['cursor_after'] = self.word(self.cursor)
            allocation['requested_end'] = allocation['address'] + allocation['size']
            # This allocator is a monotonic bump allocator; cursor gives its actual reservation.
            allocation['reserved_end'] = allocation['cursor_after'] or allocation['requested_end']
            allocation['status'] = 'returned' if allocation['address'] else 'failed'
            self.emit('allocation-return', **allocation)

    def on_code(self, address, decoded):
        self.complete_return(address)
        sp = self.uc.reg_read(REGISTERS['sp'])
        if address == self.allocator:
            lr = self.uc.reg_read(REGISTERS['x30'])
            allocation = {'id': len(self.allocations) + 1, 'caller': lr - 4 if lr != RETURN else None,
                          'synthetic_invocation': lr == RETURN,
                          'return_pc': lr, 'sp': sp,
                          'size': self.uc.reg_read(REGISTERS['x0']) & 0xffffffff,
                          'cursor_before': self.word(self.cursor), 'status': 'pending'}
            self.allocations.append(allocation)
            self.pending.append(allocation)
            self.emit('allocation-call', **allocation)
        # These checked C-library wrappers receive the claimed capacity as x1.
        # Check their full extents against observed allocations, including reuse.
        if address in (0x1176ac, 0x119c4c):
            destination = self.uc.reg_read(REGISTERS['x0'])
            length = self.uc.reg_read(REGISTERS['x3'])
            self.check_range(address, destination, length, 'memcpy-destination' if address == 0x1176ac else 'memset-destination')
            if address == 0x1176ac:
                self.check_range(address, self.uc.reg_read(REGISTERS['x2']), length, 'memcpy-source')
        self.memory_operation(address)
        if decoded is not None and decoded.mnemonic in ('blr', 'br'):
            reg = decoded.op_str.strip()
            if reg in REGISTERS:
                target = self.uc.reg_read(REGISTERS[reg])
                self.emit('indirect-branch', pc=address, register=reg, target=target,
                          allocation=(self.owner(target) or {}).get('id'))

    def on_memory(self, uc, access, address, size, value, _):
        pc = uc.reg_read(REGISTERS['pc'])
        operation = 'write' if access == unicorn.UC_MEM_WRITE else 'read'
        self.check_range(pc, address, size, operation)
        if operation == 'write':
            for slot in list(self.pointer_slots):
                if address < slot + 8 and address + size > slot:
                    del self.pointer_slots[slot]
                    self.pointer_provenance.pop(slot, None)
            if size == 8:
                value &= (1 << 64) - 1
                source = self.owner(address)
                target = self.owner(value)
                is_pointer = value != 0 and any(m['start'] <= value < m['end'] for m in self.emulator.maps)
                if source or target or is_pointer and self.arena[0] <= value < sum(self.arena):
                    self.emit('pointer-sized-write', pc=pc, address=address, value=value,
                              allocation=(source or {}).get('id'), target_allocation=(target or {}).get('id'),
                              pointer_candidate=is_pointer, provenance='firmware-instruction',
                              storage_mapping=(self.emulator.mapping(address) or {}).get('name'),
                              target_mapping=(self.emulator.mapping(value) or {}).get('name'))
                if is_pointer:
                    self.pointer_slots[address] = value
                    self.pointer_provenance[address] = 'firmware-instruction'
        else:
            for slot, target in self.pointer_slots.items():
                if address < target + 1 and address + size > target:
                    self.emit('pointer-target-access', pc=pc, address=address, size=size,
                              slot=slot, target=target)
                if address < slot + 8 and address + size > slot:
                    self.emit('pointer-slot-read', pc=pc, address=address, size=size,
                              slot=slot, value=target)

    def report(self):
        return {'arena': {'address': self.arena[0], 'size': self.arena[1]},
                'allocator': self.allocator, 'cursor_address': self.cursor, 'cursor': self.word(self.cursor),
                'top': self.word(self.top), 'scratch_pointer': self.word(BMP_SCRATCH),
                'allocations': [dict(item) for item in self.allocations], 'violation_count': self.violation_count,
                'event_count': self.event_count, 'events': [dict(item) for item in self.events],
                'events_truncated': self.event_count > len(self.events),
                'checkpoint_snapshots': list(self.checkpoint_snapshots),
                'checkpoints_truncated': self.checkpoint_count > len(self.checkpoint_snapshots),
                'pointer_slots': self.snapshot()['pointer_slots'],
                'limitations': ['Only allocations observed through this allocator are owned.',
                                'Requested bytes and allocator alignment padding are reported separately.',
                                'Range checks cannot prove pointer provenance for accesses wholly inside a different allocation.',
                                'Pointer-sized values and target accesses are candidates, not proven data flow.']}


def integer(value):
    return int(value, 0)


def region(value):
    try:
        address, size = (integer(part) for part in value.split(':'))
        if address < 0 or size <= 0 or address + size > 1 << 64:
            raise ValueError()
        return address, size
    except ValueError as error:
        raise argparse.ArgumentTypeError('expected ADDRESS:SIZE (decimal or 0x hex)') from error


def dimensions(value):
    try:
        width, height = (int(part) for part in value.lower().split('x'))
        if not 0 < width <= 4096 or not 0 < height <= 4096:
            raise ValueError()
        return width, height
    except ValueError as error:
        raise argparse.ArgumentTypeError('expected WIDTHxHEIGHT, each dimension 1..4096') from error


def save_allocations(output, allocations):
    def address(value):
        return hex(value) if value is not None else '-'
    rows = ['Allocation\tCaller\tSize\tAddress\tCursor before\tCursor after\tStatus']
    for item in allocations:
        rows.append('\t'.join((f"{item['id']:04d}", address(item['caller']), hex(item['size']),
                               address(item.get('address')), address(item['cursor_before']),
                               address(item.get('cursor_after')), item['status'])))
    (output / 'allocations.tsv').write_text('\n'.join(rows) + '\n')


class FastbootEmulator:
    def __init__(self, image, base=0):
        if not image or base < 0 or base % PAGE:
            raise ValueError('image must be nonempty and base must be nonnegative and page aligned')
        if platform.system() == 'Darwin' and platform.machine() == 'arm64':
            # Unicorn falls back to MRS CTR_EL0 when this query is blocked.
            # On Apple Silicon that fallback can SIGILL before Python can catch it.
            check = subprocess.run(['/usr/sbin/sysctl', '-n', 'hw.cachelinesize'],
                                   capture_output=True, timeout=5, check=False)
            if check.returncode:
                raise ValueError('sandbox blocks hw.cachelinesize; run with approval outside '
                                 'the sandbox or from a normal terminal (Unicorn otherwise SIGILLs)')
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        self.disassembler = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.maps = []
        self.map(base, len(image), 'image', unicorn.UC_PROT_ALL)
        self.uc.mem_write(base, image)
        self.map(SCRATCH, PAGE, 'synthetic arguments')
        self.map(STACK, STACK_SIZE, 'synthetic stack')
        self.map(RETURN, PAGE, 'return sentinel', unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)
        self.uc.reg_write(REGISTERS['sp'], STACK + STACK_SIZE)
        self.uc.reg_write(REGISTERS['x30'], RETURN)
        self.uc.reg_write(arm64.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.sha256 = hashlib.sha256(image).hexdigest()
        self.base = base
        self.heap = None
        self.assumptions = ['Raw little-endian AArch64 image; no relocations applied.',
                            'Image padding is mapped as captured; no firmware initialization inferred.',
                            'Synthetic stack, arguments, return address, and enabled FP/SIMD.',
                            'No peripheral or secure-monitor model; unknown accesses stop execution.']

    def require_stock(self):
        if self.sha256 != STOCK_SHA256 or self.base != 0:
            raise ValueError('this profile requires the exact stock ANE-LX1 image at base 0')

    def initialize_bss(self):
        self.require_stock()
        # The startup clear routine at 0x320 loads these bounds from its literal pool.
        start, end = struct.unpack('<QQ', self.uc.mem_read(0x3d8, 16))
        if (start, end) != (0x292540, 0x2bbcb0):
            raise ValueError('unexpected BSS bounds')
        self.uc.mem_write(start, bytes(end - start))
        self.assumptions.append(f'Applied startup BSS clear [{start:#x}, {end:#x}) '
                                'from literals at 0x3d8 and 0x3e0; no module initializers executed.')

    def watch_heap(self, checkpoints=(), stop_on_oob=False):
        self.require_stock()
        self.heap = HeapMonitor(self, (HEAP_BASE, HEAP_SIZE), checkpoints=checkpoints,
                                stop_on_oob=stop_on_oob)
        return self.heap

    def bmp_profile(self, bitmap=(2, 1), panel=(8, 8), bytes_per_pixel=1):
        self.require_stock()
        width, height = bitmap
        panel_width, panel_height = panel
        if not (0 < width <= panel_width <= 4096 and 0 < height <= panel_height <= 4096
                and 1 <= bytes_per_pixel <= 4 and panel_width * panel_height * bytes_per_pixel <= 16 << 20):
            raise ValueError('bitmap must fit panel, dimensions must be 1..4096, bpp 1..4, buffer <=16 MiB')
        self.initialize_bss()
        self.map(HEAP_BASE, HEAP_SIZE, 'native allocator arena')
        buffer = 0x35000000
        self.map(buffer, panel_width * panel_height * bytes_per_pixel, 'synthetic logo buffer')
        self.uc.mem_write(SCRATCH + 0x148, struct.pack('<Q', SCRATCH + 0x200))
        self.uc.mem_write(SCRATCH + 0x204, struct.pack('<II', panel_width, panel_height))
        display_cell = struct.unpack('<Q', self.uc.mem_read(0x291a88, 8))[0]
        self.uc.mem_write(display_cell, struct.pack('<Q', SCRATCH + 0x100))
        log_level = struct.unpack('<Q', self.uc.mem_read(0x291740, 8))[0]
        self.uc.mem_write(log_level, struct.pack('<I', 0))
        self.assumptions.append(f'Isolated centering: synthetic {panel_width}x{panel_height} '
                                f'panel, {width}x{height} decoded pixels, {bytes_per_pixel} bytes/pixel; '
                                'display structure and log level supplied, heap initially empty. '
                                'No BMP parser, storage, hardware routines, or module initialization run.')
        self.prepare_bitmap(bitmap, bytes_per_pixel)
        self.watch_heap(checkpoints=(*LOGO_CHECKPOINTS, 0x4ef18, 0x4ec8c))
        return BMP_CENTER

    def logo_center_profile(self, bitmap=(2, 1), panel=(8, 8), bytes_per_pixel=1):
        self.bmp_profile(bitmap, panel, bytes_per_pixel)
        # Enter the real caller so its BL at 0x4f020 returns naturally to 0x4f024.
        # Setting LR to 0x4f028 at centering entry would fabricate that evidence.
        self.uc.mem_write(0x293604, struct.pack('<I', 1))
        self.assumptions.append('Isolated caller replay at 0x4ef74; synthetic display-ready flag. '
                                'Module initialization and hisi_display_logo entry are not inferred.')
        return 0x4ef74

    def prepare_bitmap(self, bitmap, bytes_per_pixel=1):
        width, height = bitmap
        self.uc.mem_write(SCRATCH, struct.pack('<IIIII', 0, 0, width - 1, height - 1, bytes_per_pixel))
        self.uc.mem_write(0x35000000, bytes([0x5a]) * (width * height * bytes_per_pixel))
        self.uc.reg_write(REGISTERS['x0'], SCRATCH)
        self.uc.reg_write(REGISTERS['x1'], 0x35000000)
        self.uc.reg_write(REGISTERS['x30'], RETURN)

    def map(self, address, size, name='explicit RAM', permissions=unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE):
        if address < 0 or size <= 0 or address + size > 1 << 64:
            raise ValueError('invalid memory range')
        start = address & -PAGE
        end = (address + size + PAGE - 1) & -PAGE
        if any(start < item['end'] and end > item['start'] for item in self.maps):
            raise ValueError(f'memory range overlaps an existing mapping: {start:#x}:{end-start:#x}')
        self.uc.mem_map(start, end - start, permissions)
        self.maps.append({'start': start, 'end': end, 'name': name, 'permissions': permissions})

    def mapping(self, address):
        return next((item for item in reversed(self.maps) if item['start'] <= address < item['end']), None)

    def permission_profile(self, command, fblock=0, userlock=1, frp=1):
        if self.sha256 != STOCK_SHA256 or self.base != 0:
            raise ValueError('permission profile requires the exact stock ANE-LX1 image at base 0')
        encoded = command.encode('ascii')
        if b'\0' in encoded or len(encoded) >= PAGE:
            raise ValueError('command must be a NUL-free ASCII string shorter than 4096 bytes')
        state = struct.unpack('<Q', self.uc.mem_read(0x292000, 8))[0]
        for offset, value in ((0x214, fblock), (0x218, userlock), (0x224, frp)):
            self.uc.mem_write(state + offset, struct.pack('<I', value))
        self.uc.mem_write(SCRATCH, encoded + b'\0')
        self.uc.reg_write(REGISTERS['x0'], SCRATCH)
        self.assumptions.append(f'Synthetic permission inputs: command={command!r}, '
                                f'FBLOCK={fblock}, USERLOCK={userlock}, FRP={frp}. '
                                'These are supplied inputs, not live device measurements.')
        return 0x2abb8

    def run(self, entry, until=None, count=100000, timeout_ms=2000, trace=None, events=None,
            breakpoints=(), watches=(), observation_stream=None, trace_registers=False,
            diagnostic_limit=20000, checkpoint_callback=None):
        if (not 0 <= entry < 1 << 64 or entry % 4
                or (until is not None and (not 0 <= until < 1 << 64 or until % 4))):
            raise ValueError('entry and until must be nonnegative, aligned AArch64 addresses')
        if not 0 < count <= 10000000 or not 0 < timeout_ms <= 60000:
            raise ValueError('count must be 1..10000000 and timeout-ms must be 1..60000')
        recent = deque(maxlen=32)
        breakpoints = set(breakpoints)
        if any(not 0 <= pc < 1 << 64 or pc % 4 for pc in breakpoints):
            raise ValueError('breakpoints must be aligned AArch64 addresses')
        monitor = ExecutionMonitor(self, watches, observation_stream, diagnostic_limit)
        initial_registers = {name: hex(self.uc.reg_read(reg)) for name, reg in REGISTERS.items()}
        previous_registers = {}
        initial_heap = self.heap.snapshot() if self.heap else None
        state = {'instructions_observed': 0, 'stop_reason': None,
                 'memory_diagnostics': {'invalid_reads': 0, 'invalid_writes': 0, 'invalid_fetches': 0}}
        boot = {'module_initialization': 'not-observed', 'modules': [],
                'hisi_display_logo': 'not-reached', 'centering': 'not-reached'}
        stock = self.sha256 == STOCK_SHA256 and self.base == 0
        checkpoints_seen = set()
        if self.heap:
            self.heap.stream = events
            self.heap.stop_requested = False

        def capture_checkpoint(address):
            if self.heap and address in self.heap.checkpoints:
                self.heap.instruction_step = state['instructions_observed'] + 1
                self.heap.complete_return(address)
                snapshot = self.heap.checkpoint(address)
                checkpoints_seen.add(address)
                if checkpoint_callback is not None:
                    checkpoint_callback(snapshot)

        def on_code(uc, address, size, _):
            raw = bytes(uc.mem_read(address, size))
            decoded = next(self.disassembler.disasm(raw, address, count=1), None)
            if stock:
                if address == DISPLAY_LOGO:
                    boot['hisi_display_logo'] = 'reached'
                if address == BMP_CENTER:
                    boot['centering'] = 'reached'
                if address == 0x1113ac:
                    descriptor = uc.reg_read(REGISTERS['x22'])
                    name_pointer = struct.unpack('<Q', uc.mem_read(descriptor, 8))[0]
                    name = bytes(uc.mem_read(name_pointer, 128)).split(b'\0', 1)[0].decode('ascii', 'replace')
                    boot['module_initialization'] = 'in-progress'
                    boot['modules'].append({'name': name, 'descriptor': hex(descriptor),
                                            'entry': hex(uc.reg_read(REGISTERS['x1'])), 'status': 'entered'})
                if address == 0x1113b0 and boot['modules']:
                    module = boot['modules'][-1]
                    module['return_value'] = hex(uc.reg_read(REGISTERS['x0']))
                    module['status'] = 'returned' if module['return_value'] == '0x0' else 'failed'
                if address == 0x1113e8:
                    boot['module_initialization'] = 'completed'
            capture_checkpoint(address)
            if address in breakpoints or address == RETURN or address == until:
                if self.heap:
                    self.heap.complete_return(address)
                state['stop_reason'] = ('breakpoint' if address in breakpoints else
                                        'returned' if address == RETURN else 'until')
                uc.emu_stop()
                return
            if self.heap:
                # Checkpoint capture is managed above, including stop boundaries.
                self.heap.instruction_step = state['instructions_observed'] + 1
                self.heap.on_code(address, decoded)
                if self.heap.stop_requested:
                    state['stop_reason'] = 'heap-bounds-violation'
                    return
            state['instructions_observed'] += 1
            monitor.on_code(address, decoded)
            item = {'step': state['instructions_observed'], 'pc': hex(address), 'bytes': raw.hex(),
                    'instruction': f'{decoded.mnemonic} {decoded.op_str}'.strip() if decoded else '<invalid>'}
            if trace_registers:
                registers = {name: hex(uc.reg_read(reg)) for name, reg in REGISTERS.items()}
                item['register_changes'] = {name: value for name, value in registers.items()
                                            if previous_registers.get(name) != value}
                previous_registers.update(registers)
            recent.append(item)
            if trace is not None:
                trace.write(json.dumps(item) + '\n')
            if decoded is not None and decoded.mnemonic in TRAPS:
                state['stop_reason'] = 'trap'
                state['trap'] = item
                uc.emu_stop()

        def on_fault(uc, access, address, size, value, _):
            state['stop_reason'] = 'memory-fault'
            operation = ACCESS_NAMES.get(access, 'unknown')
            counter = ('invalid_reads' if operation.startswith('read') else
                       'invalid_writes' if operation.startswith('write') else 'invalid_fetches')
            state['memory_diagnostics'][counter] += 1
            state['fault'] = {'access': access, 'address': hex(address), 'size': size,
                              'operation': operation, 'pc': hex(uc.reg_read(REGISTERS['pc'])),
                              'value': hex(value & ((1 << 64) - 1))}
            return False

        hooks = [self.uc.hook_add(unicorn.UC_HOOK_CODE, on_code),
                 self.uc.hook_add(unicorn.UC_HOOK_MEM_INVALID, on_fault),
                 self.uc.hook_add(unicorn.UC_HOOK_BLOCK, monitor.on_block)]
        if watches:
            hooks.append(self.uc.hook_add(unicorn.UC_HOOK_MEM_READ | unicorn.UC_HOOK_MEM_WRITE,
                                          monitor.on_memory))
        if self.heap:
            hooks.append(self.uc.hook_add(unicorn.UC_HOOK_MEM_READ | unicorn.UC_HOOK_MEM_WRITE,
                                          self.heap.on_memory))
        try:
            self.uc.emu_start(entry, RETURN if until is None else until,
                              timeout=timeout_ms * 1000, count=count)
        except unicorn.UcError as error:
            state['stop_reason'] = state['stop_reason'] or 'emulator-error'
            state['error'] = str(error)
        finally:
            # Unicorn's until boundary may stop before the code hook sees the sentinel.
            try:
                if self.heap:
                    self.heap.complete_return(self.uc.reg_read(REGISTERS['pc']))
                    pc = self.uc.reg_read(REGISTERS['pc'])
                    if pc == until and pc not in checkpoints_seen:
                        capture_checkpoint(pc)
            finally:
                if self.heap:
                    self.heap.stream = None
                for hook in hooks:
                    self.uc.hook_del(hook)
        pc = self.uc.reg_read(REGISTERS['pc'])
        if self.heap and self.heap.stop_requested:
            state['stop_reason'] = 'heap-bounds-violation'
        state['memory_diagnostics']['allocation_overruns'] = (
            self.heap.violation_count - initial_heap['violation_count'] if self.heap else 0)
        if state['stop_reason'] is None:
            if pc == RETURN:
                state['stop_reason'] = 'returned'
            elif until is not None and pc == until:
                state['stop_reason'] = 'until'
            elif self.uc.query(unicorn.UC_QUERY_TIMEOUT):
                state['stop_reason'] = 'timeout'
            else:
                state['stop_reason'] = 'instruction-limit'
        if state['stop_reason'] not in ('returned', 'until', 'breakpoint'):
            try:
                raw = bytes(self.uc.mem_read(pc, 4))
                decoded = next(self.disassembler.disasm(raw, pc, count=1), None)
                instruction = f'{decoded.mnemonic} {decoded.op_str}'.strip() if decoded else '<invalid>'
            except unicorn.UcError:
                instruction = '<unmapped>'
            state['failure_context'] = {'pc': hex(pc), 'instruction': instruction,
                                        'last_events': list(monitor.events)[-8:]}
        return {'schema': 1, 'image_sha256': self.sha256, 'image_base': hex(self.base),
                'entry': hex(entry), 'until': hex(until) if until is not None else None,
                'limits': {'instructions': count, 'timeout_ms': timeout_ms},
                'versions': {'unicorn': unicorn.__version__, 'capstone': capstone.__version__},
                'assumptions': list(self.assumptions), 'memory_maps': [dict(m) for m in self.maps],
                'initial_registers': initial_registers, 'diagnostics': monitor.report(),
                **({'boot_progress': boot} if stock else {}),
                'breakpoints': [hex(pc) for pc in sorted(breakpoints)],
                **({'heap': self.heap.report(), 'initial_heap': initial_heap} if self.heap else {}),
                **state, 'registers': {name: hex(self.uc.reg_read(reg)) for name, reg in REGISTERS.items()},
                'recent_instructions': list(recent)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=DEFAULT_IMAGE)
    parser.add_argument('--base', type=integer, default=0, help='raw image mapping base; no relocation')
    parser.add_argument('--profile', choices=('raw', 'permission', 'bmp-center', 'logo-center'), default='raw')
    parser.add_argument('--entry', type=integer, help='start address; raw default 0, permission 0x2abb8')
    parser.add_argument('--until', type=integer, help='stop before this address')
    parser.add_argument('--count', type=int, default=100000)
    parser.add_argument('--timeout-ms', type=int, default=2000)
    parser.add_argument('--map', type=region, action='append', default=[], metavar='ADDRESS:SIZE')
    parser.add_argument('--write', action='append', default=[], metavar='ADDRESS:HEXBYTES')
    parser.add_argument('--reg', action='append', default=[], metavar='NAME=VALUE')
    parser.add_argument('--dump', type=region, action='append', default=[], metavar='ADDRESS:SIZE')
    parser.add_argument('--command', default='flash:kernel', help='permission-profile command input')
    parser.add_argument('--fblock', type=int, choices=(0, 1), default=0)
    parser.add_argument('--userlock', type=int, choices=(0, 1), default=1)
    parser.add_argument('--frp', type=int, choices=(0, 1), default=1)
    parser.add_argument('--trace', action='store_true', help='save every observed instruction as trace.jsonl')
    parser.add_argument('--trace-registers', action='store_true', help='include register changes before each instruction')
    parser.add_argument('--breakpoint', type=integer, action='append', default=[], help='stop before PC (repeatable)')
    parser.add_argument('--watch', type=region, action='append', default=[], metavar='ADDRESS:SIZE',
                        help='record guest reads/writes intersecting a range')
    parser.add_argument('--init-bss', action='store_true', help='apply exact stock startup BSS clear')
    parser.add_argument('--heap-watch', action='store_true', help='observe stock allocator and heap accesses')
    parser.add_argument('--checkpoint', type=integer, action='append', default=[], help='record heap state at PC')
    parser.add_argument('--stop-on-oob', action='store_true', help='stop when heap tracking detects a bounds violation')
    parser.add_argument('--bitmap', type=dimensions, default=(2, 1), metavar='WIDTHxHEIGHT')
    parser.add_argument('--panel', type=dimensions, default=(8, 8), metavar='WIDTHxHEIGHT')
    parser.add_argument('--bytes-per-pixel', type=int, default=1)
    parser.add_argument('--output', type=Path, help='new output directory; default private temporary directory')
    args = parser.parse_args()
    try:
        image = args.image.expanduser().read_bytes()
        emulator = FastbootEmulator(image, args.base)
        entry = args.base
        if args.profile == 'permission':
            entry = emulator.permission_profile(args.command, args.fblock, args.userlock, args.frp)
        elif args.profile == 'bmp-center':
            entry = emulator.bmp_profile(args.bitmap, args.panel, args.bytes_per_pixel)
        elif args.profile == 'logo-center':
            entry = emulator.logo_center_profile(args.bitmap, args.panel, args.bytes_per_pixel)
        if args.init_bss and args.profile not in ('bmp-center', 'logo-center'):
            emulator.initialize_bss()
        if args.heap_watch and emulator.heap is None:
            emulator.watch_heap()
        if emulator.heap:
            emulator.heap.checkpoints.update(args.checkpoint)
            emulator.heap.stop_on_oob = args.stop_on_oob
        if args.entry is not None:
            entry = args.entry
        for address, size in args.map:
            emulator.map(address, size)
            emulator.assumptions.append(f'Explicit zero-filled RAM at {address:#x}, size {size:#x}.')
        for assignment in args.write:
            address, data = assignment.split(':', 1)
            data = bytes.fromhex(data)
            emulator.uc.mem_write(integer(address), data)
            emulator.assumptions.append(f'Explicit in-memory write at {address}, {len(data)} bytes: {data.hex()}.')
        for assignment in args.reg:
            name, value = assignment.lower().split('=', 1)
            if name not in REGISTERS or name == 'pc':
                raise ValueError('use x0..x30, sp, nzcv for --reg, and --entry for pc')
            value = integer(value)
            if not 0 <= value < 1 << 64:
                raise ValueError('register values must fit an unsigned 64-bit integer')
            emulator.uc.reg_write(REGISTERS[name], value)
            emulator.assumptions.append(f'Explicit initial register {name}={value:#x}.')
        if args.output is None:
            output = Path(tempfile.mkdtemp(prefix='ame-fastboot-unicorn-'))
        else:
            output = args.output.expanduser().resolve()
            output.mkdir(parents=True, mode=0o700, exist_ok=False)
        trace_context = (output / 'trace.jsonl').open('w') if args.trace or args.trace_registers else nullcontext(None)
        event_context = (output / 'events.jsonl').open('w') if emulator.heap else nullcontext(None)
        with trace_context as trace, event_context as events, (output / 'execution.jsonl').open('w') as observations:
            report = emulator.run(entry, args.until, args.count, args.timeout_ms, trace, events,
                                  breakpoints=args.breakpoint, watches=args.watch,
                                  observation_stream=observations, trace_registers=args.trace_registers)
        report.update(image=str(args.image.expanduser().resolve()), profile=args.profile)
        report['dumps'] = []
        for index, (address, size) in enumerate(args.dump):
            name = f'dump-{index}-{address:x}.bin'
            try:
                data = bytes(emulator.uc.mem_read(address, size))
                (output / name).write_bytes(data)
                report['dumps'].append({'address': hex(address), 'size': size, 'file': name})
            except unicorn.UcError as error:
                report['dumps'].append({'address': hex(address), 'size': size, 'error': str(error)})
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f"{report['stop_reason']}: {report['instructions_observed']} instructions observed, "
              f"pc={report['registers']['pc']}, x0={report['registers']['x0']}")
        print(f'Report: {output / "report.json"}')
        if emulator.heap:
            print(f'Heap: {len(emulator.heap.allocations)} allocations, '
                  f'{emulator.heap.violation_count} bounds violations')
            save_allocations(output, emulator.heap.allocations)
        if (report['stop_reason'] not in ('returned', 'until', 'breakpoint')
                or report.get('heap', {}).get('violation_count', 0)
                or any('error' in d for d in report['dumps'])):
            return 1
        return 0
    except (OSError, ValueError, subprocess.TimeoutExpired, unicorn.UcError) as error:
        parser.exit(2, f'fastboot emulation: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
