#!/usr/bin/env python3
"""Read retained native Anko logs from TWRP; decode PCs with the packaged ELF.

Read-only on the phone. Raw records and identifiers stay in the private output
directory. This command never flashes or reboots the device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import struct

ROOT = Path(__file__).resolve().parents[2]
NAMES = ['console-ramoops-0', 'pmsg-ramoops-0',
         'dmesg-ramoops-0', 'dmesg-ramoops-1', 'dmesg-ramoops-2']


def matches_run(text, diagnostic):
    expected = diagnostic.get('run_tag')
    if not expected: return True
    start = text.rfind('AMEANKO1 ')
    return start >= 0 and re.search(r'^RUN tag=' + re.escape(expected) + r'$', text[start:], re.M) is not None


def extract_record(data, offset):
    if offset < 0 or offset + 12 > len(data):
        raise ValueError('record header is outside the backup')
    magic, cursor, size = struct.unpack_from('<3I', data, offset)
    if magic != 0x43474244 or cursor != size or not 0 < size <= 0x80000 - 12:
        raise ValueError('expected a bounded, nonwrapped native persistent_ram header')
    if size > len(data) - offset - 12: raise ValueError('record is truncated')
    record = data[offset + 12:offset + 12 + size]
    if record.startswith(b'====0.0-D\n'): record = record[10:]
    if not record.startswith(b'AMEANKO1 '): raise ValueError('selected record does not begin with the native trace tag')
    record.decode('utf-8')
    return record


def dfx_records(data, diagnostic):
    if not diagnostic.get('run_tag'):
        raise ValueError('automatic DFX matching requires a package run_tag; explicitly select older records offline')
    found = []; offset = data.find(b'DBGC')
    while offset >= 0:
        try:
            raw = extract_record(data, offset)
            if matches_run(raw.decode('utf-8'), diagnostic): found.append((offset, raw))
        except ValueError: pass
        offset = data.find(b'DBGC', offset + 4)
    return found


def decode(text, elf=None):
    offset = text.rfind('AMEANKO1 ')
    if offset < 0:
        return None
    text = text[offset:]
    stages = [{'id': int(n), 'name': name, 'tick': int(tick, 16), 'cpu': int(cpu or 0)} for n, name, tick, cpu in
              re.findall(r'BOOT (\d+) ([\w.]+) tick=(0x[\da-f]+)(?: cpu=(\d+))?', text)]
    faults = []
    for match in re.finditer(
            r'!FAULT last=(\d+) esr=(0x[\da-f]+) pc=(0x[\da-f]+) far=(0x[\da-f]+) sp=(0x[\da-f]+)(?: cpu=(\d+))?', text):
        last, esr, pc, far, sp, cpu = match.groups()
        vectors = list(re.finditer(r'^VECTOR (\d+)(?: saved_pstate=(0x[\da-f]+))?$', text[:match.start()], re.M))
        vector = int(vectors[-1].group(1)) if vectors else None
        saved_pstate = vectors[-1].group(2) if vectors else None
        kind = ('sync', 'irq', 'fiq', 'serror')[vector % 4] if vector is not None else 'unknown'
        syndrome_valid = kind not in ('irq', 'fiq')
        syndrome = int(esr, 16)
        fault = dict(last=int(last), cpu=int(cpu or 0), esr=esr, pc=pc, far=far, sp=sp,
                     vector=vector, kind=kind, saved_pstate=saved_pstate, syndrome_valid=syndrome_valid,
                     exception_class=(syndrome >> 26) & 63 if syndrome_valid else None,
                     iss=syndrome & 0x1ffffff if syndrome_valid else None)
        tool = shutil.which('llvm-addr2line') or '/opt/homebrew/opt/llvm/bin/llvm-addr2line'
        if elf and Path(tool).is_file():
            result = subprocess.run([tool, '-e', str(elf), '-f', '-C', pc],
                                    capture_output=True, text=True, timeout=10, check=True)
            fault['symbol'] = result.stdout.strip()
        faults.append(fault)
    user_faults = [dict(pid=int(pid), tid=int(tid), esr=esr, pc=pc, far=far, sp=sp,
                        exception_class=(int(esr, 16) >> 26) & 63, iss=int(esr, 16) & 0x1ffffff)
                   for pid, tid, esr, pc, far, sp in re.findall(
                       r'!USERFAULT pid=(\d+) tid=(\d+) esr=(0x[\da-f]+) pc=(0x[\da-f]+) far=(0x[\da-f]+) sp=(0x[\da-f]+)', text)]
    online = re.findall(r'smp ready online=(\d+) mask=([\da-f]+)', text)
    ressha = [dict(cpu=int(cpu), switches=int(switches), timer_preempts=int(preempts), ticks=int(ticks), ipis=int(ipis))
               for cpu, switches, preempts, ticks, ipis in re.findall(
                   r'RESSHA cpu=(\d+) switches=(\d+) preempts=(\d+) ticks=(\d+) ipis=(\d+)', text)]
    workload = [dict(cpu=int(cpu), observed=int(observed), rounds=int(rounds)) for cpu, observed, rounds in
                re.findall(r'SMP_WORK cpu=(\d+) observed=(\d+) rounds=(\d+)', text)]
    display = dict(
        phases=re.findall(r'DISPLAY begin phase=([\w.]+)', text),
        candidates=[dict(channel=int(channel, 16), rejection=reason) for channel, reason in
                    re.findall(r'DISPLAY candidate channel=(0x[\da-f]+) +reject=([\w.]+)', text)],
        painted='DISPLAY paint.done label=ANKO' in text,
        skipped='DISPLAY paint=skip no_verified_scanout' in text)
    replay = re.findall(r'DISPLAY console.replayed bytes=(\d+)', text)
    screen_ready = re.findall(r'DISPLAY console.services_ready characters=(\d+)', text)
    display['console'] = dict(ready='DISPLAY console.ready ' in text,
                              replayed_bytes=int(replay[-1]) if replay else 0,
                              characters_at_readiness=int(screen_ready[-1]) if screen_ready else 0)
    run_tags = re.findall(r'^RUN tag=([A-Za-z0-9_.-]+)$', text, re.M)
    firmware_fiq = []
    for phase, word, tick in re.findall(r'^FIQ_SOURCE phase=([\w.]+) word=(0x[\da-f]+) tick=(0x[\da-f]+)$', text, re.M):
        reason = int(word, 16) & 0xff
        firmware_fiq.append(dict(phase=phase, word=word, tick=int(tick, 16), reason_code=reason,
                                 kind={76: 'watchdog', 163: 'ddr.security'}.get(reason, 'unknown')))
    watchdog = [dict(phase=phase, status=int(status), id=int(pid, 16), control=int(control, 16),
                     load=int(load, 16), value=int(value, 16), raw_status=int(ris, 16), feeds=int(feeds))
                for phase, status, pid, control, load, value, ris, feeds in re.findall(
                    r'^WATCHDOG phase=([\w.]+) status=(\d+) id=(0x[\da-f]+) control=(0x[\da-f]+) load=(0x[\da-f]+) value=(0x[\da-f]+) ris=(0x[\da-f]+) feeds=(\d+)$', text, re.M)]
    return dict(stages=stages, last_checkpoint=stages[-1] if stages else None, user_faults=user_faults,
                online_cpus=int(online[-1][0]) if online else None,
                online_mask=int(online[-1][1],16) if online else None, ressha=ressha, workload=workload, display=display,
                faults=faults, firmware_fiq=firmware_fiq, watchdog=watchdog,
                panic=re.findall(r'!PANIC last=\d+ [^\n]+', text),
                reset='RESET nmi=0x4' in text, run_tag=run_tags[-1] if run_tags else None,
                services_ready='BOOT 36 services.ready' in text, text=text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--targets', type=Path, default=ROOT / 'out/anne-device-private/adb-targets.json')
    parser.add_argument('--adb', type=Path, default=ROOT / 'out/host-tools/platform-tools/adb')
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--offline', action='store_true', help='decode records already in output/pstore')
    parser.add_argument('--dfx-fallback', action='store_true', help='read the diagnostic backup if pstore has no matching tagged trace')
    args = parser.parse_args()
    manifest = json.loads((args.package / 'manifest.json').read_text())
    elf = args.package / 'kernel.elf'
    expected = manifest['diagnostic']['kernel_elf_sha256']
    if hashlib.sha256(elf.read_bytes()).hexdigest() != expected:
        raise SystemExit('Packaged ELF checksum mismatch')
    args.output.mkdir(parents=True, exist_ok=True, mode=0o700)
    records_dir = args.output / 'pstore'
    records_dir.mkdir(exist_ok=True, mode=0o700)
    if not args.offline:
        serial = json.loads(args.targets.read_text())[0]['serial']
        def adb(*command):
            try:
                result = subprocess.run([str(args.adb), '-s', serial, *command],
                                        capture_output=True, timeout=30)
            except subprocess.TimeoutExpired:
                raise SystemExit('Recovery ADB timed out') from None
            if result.returncode:
                raise SystemExit('Recovery ADB read failed')
            return result.stdout
        if adb('get-state').strip() not in (b'device', b'recovery'):
            raise SystemExit('Intended recovery device is not connected')
        if not adb('shell', 'getprop ro.twrp.version').strip():
            raise SystemExit('Expected TWRP; refusing to collect from another boot mode')
        digest = adb('shell', 'sha256sum /dev/block/platform/hi_mci.0/by-name/kernel').decode().split()[0]
        if digest != manifest['restore_kernel_sha256']:
            raise SystemExit('Stock kernel is not verified restored')
        (args.output / 'stock-verification.json').write_text(json.dumps({'sha256': digest}) + '\n')
        available = set(adb('shell', 'ls /sys/fs/pstore').decode().split())
        for name in NAMES:
            if name in available:
                (records_dir / name).write_bytes(adb('exec-out', 'cat', '/sys/fs/pstore/' + name))
    records = []
    for name in NAMES:
        path = records_dir / name
        if path.exists():
            text = path.read_text(errors='replace')
            if not matches_run(text, manifest['diagnostic']): continue
            record = decode(text, elf)
            if record:
                records.append(dict(source=name, **record))
    if not records and args.dfx_fallback:
        dfx_path = args.output / 'dfx-private.bin'
        if not args.offline:
            if not manifest['diagnostic'].get('run_tag'):
                raise SystemExit('DFX fallback requires a unique package run_tag')
            sectors = int(adb('shell', 'cat /sys/class/block/mmcblk0p40/size').strip())
            size = sectors * 512
            if not 0 < size <= 64 * 1024 * 1024: raise SystemExit('Unexpected DFX partition size')
            backup = adb('exec-out', 'cat', '/dev/block/platform/hi_mci.0/by-name/dfx')
            if len(backup) != size: raise SystemExit('DFX backup is incomplete')
            dfx_path.write_bytes(backup)
        if dfx_path.stat().st_size > 64 * 1024 * 1024: raise SystemExit('DFX backup exceeds size limit')
        backup = dfx_path.read_bytes()
        dfx_dir = args.output / 'dfx-records'; dfx_dir.mkdir(exist_ok=True, mode=0o700)
        extraction = []
        for offset, raw in dfx_records(backup, manifest['diagnostic']):
            name = f'record-{offset:08x}.txt'; (dfx_dir / name).write_bytes(raw)
            records.append(dict(source=f'dfx@{offset:#x}', **decode(raw.decode('utf-8'), elf)))
            extraction.append(dict(header_offset=offset, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
        (args.output / 'dfx-summary.json').write_text(json.dumps(dict(
            backup_sha256=hashlib.sha256(backup).hexdigest(), run_tag=manifest['diagnostic']['run_tag'],
            records=extraction), indent=2) + '\n')
    (args.output / 'decoded.json').write_text(json.dumps(records, indent=2) + '\n')
    print(json.dumps([{'source': r['source'], 'last_checkpoint': r['last_checkpoint'],
                       'faults': r['faults'], 'user_faults': r['user_faults'], 'panic': r['panic'], 'reset': r['reset'],
                       'services_ready': r['services_ready'], 'online_cpus':r['online_cpus'],
                       'online_mask':r['online_mask'], 'ressha':r['ressha'], 'workload':r['workload'],
                       'display':r['display'], 'firmware_fiq':r['firmware_fiq'],
                       'watchdog':r['watchdog']} for r in records], indent=2))
    if not records:
        raise SystemExit('No AMEANKO1 record retained; previous probe logs are not this kernel')


if __name__ == '__main__':
    main()
