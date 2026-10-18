#!/usr/bin/env python3
"""Run comparable two-process IPC workloads; retain raw samples and provenance.

Examples:
  ipc-bench.py linux --transport seqpacket --output out/ipc-linux
  ipc-bench.py ame --include-spin --output out/ipc-ame
  ipc-bench.py compare old/results.json new/results.json
"""
import argparse
import contextlib
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import signal
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from benchmarks.ipc import results


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def version(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        return None


def cpu_model():
    text = read_optional('/proc/cpuinfo') or ''
    for name in ('model name', 'Hardware', 'Processor'):
        for line in text.splitlines():
            if line.split(':', 1)[0].strip() == name:
                return line.split(':', 1)[1].strip()
    return platform.processor() or platform.machine()


def topology(cpu):
    base = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
    return {name: read_optional(base / name) for name in
            ('physical_package_id', 'core_id', 'thread_siblings_list')}


def host_metadata():
    return {'host_identity': platform.node(), 'host_isa': platform.machine(),
            'host_os': platform.platform(), 'host_online_cpus': os.cpu_count(), 'host_cpu_model': cpu_model(),
            'allowed_cpus': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
            'cgroup_cpu_max': read_optional('/sys/fs/cgroup/cpu.max'),
            'host_dmi_product': read_optional('/sys/class/dmi/id/product_name'),
            'host_clocksource': read_optional('/sys/devices/system/clocksource/clocksource0/current_clocksource')}


def execute(command, log, timeout=300):
    with Path(log).open('w') as output:
        process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            # Linux's benchmark forks a peer; kill the process group on timeout
            # or interruption so a failed test cannot leave a runnable server.
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise
    if code:
        raise RuntimeError(f'command failed ({code}); see {log}')


def configuration(args):
    sizes = [int(value) for value in args.payloads.split(',')]
    results.require(len(sizes) == len(set(sizes)) and sizes and all(size in results.PAYLOADS for size in sizes),
                    'payloads must be distinct entries from 0,8,48,256,4096')
    config = {'iterations': args.iterations, 'warmup': args.warmup, 'samples': args.samples,
              'rounds': args.rounds, 'window': args.window,
              'payload_mask': sum(1 << results.PAYLOADS.index(size) for size in sizes),
              'include_spin': args.include_spin}
    for name, lo, hi in [('iterations', 16, 1048576), ('warmup', 1, 1048576), ('samples', 16, 16384),
                         ('rounds', 3, 31), ('window', 1, 64)]:
        results.require(lo <= config[name] <= hi, f'{name} must be in [{lo}, {hi}]')
    results.require(1 <= args.runs <= 31 and args.timeout > 0, 'invalid run count or timeout')
    return config


def output_directory(args):
    path = args.output or ROOT / 'out/ipc-results' / (args.action + '-' +
             datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6])
    path = path.resolve()
    path.mkdir(parents=True, exist_ok=False)
    return path


def contract(run):
    return {'version': 1, 'workload_sha256': run['header']['contract_sha256'],
            'peers': 'two separate address spaces with one worker each',
            'clock': 'monotonic nanoseconds via explicit syscall',
            'validation': 'every sequence and every deterministic payload byte',
            'rpc': 'one outstanding copied request and equal-sized copied echo',
            'stream': 'one-way copied messages; empty completion ACK per window including final partial window',
            'aggregate_timing': 'two clock reads per batch; setup, output and diagnostic snapshots excluded',
            'latency_timing': 'two clock reads per RPC, included and not subtracted; never one-way latency'}


def save(output, metadata, runs):
    report = {'schema': results.SCHEMA, 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'contract': contract(runs[0]), 'metadata': metadata,
              'runs': runs, 'summary': results.summarize(runs)}
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    with (output / 'summary.csv').open('w', newline='') as stream:
        fields = ['key', 'median_ns_per_operation', 'operations_per_second',
                  'payload_goodput_bytes_per_second', 'rtt_p50_ns', 'rtt_p99_ns', 'run_cv']
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in report['summary']['cases']:
            values = {field: row[field] for field in fields if field in row}
            latency = row['instrumented_rtt_ns'] or {}
            values.update(rtt_p50_ns=latency.get('p50_ns'), rtt_p99_ns=latency.get('p99_ns'))
            writer.writerow(values)
    for row in report['summary']['cases']:
        print(f"{row['key']:48s} {row['median_ns_per_operation']:11.1f} ns/op  "
              f"{row['operations_per_second']:12.0f} op/s")
    print(f'Results: {output / "results.json"}')
    for warning in report['summary']['warnings']:
        print('Measurement note: ' + warning)
    return report


def run_linux(args):
    config = configuration(args)
    output = output_directory(args)
    build = args.build.resolve(); build.mkdir(parents=True, exist_ok=True)
    binary = build / 'ipc-linux'
    common_hash = digest(ROOT / 'benchmarks/ipc/workload.h')
    flags = ['-std=c++23', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             f'-DIPC_BENCH_CONTRACT_SHA256="{common_hash}"',
             '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include')]
    command = [args.compiler, *flags, str(ROOT / 'benchmarks/ipc/linux.cc'),
               str(ROOT / 'lib/libk/printf.cc'), '-o', str(binary)]
    execute(command, output / 'build.log')
    allowed = sorted(os.sched_getaffinity(0))
    client = allowed[0] if args.client_cpu is None else args.client_cpu
    siblings = topology(client)
    remote = next((cpu for cpu in allowed if cpu != client and
                   (topology(cpu)['physical_package_id'], topology(cpu)['core_id']) !=
                   (siblings['physical_package_id'], siblings['core_id'])),
                  next((cpu for cpu in allowed if cpu != client), client))
    server = remote if args.server_cpu is None else args.server_cpu
    results.require(client in allowed and server in allowed, 'selected CPUs are outside allowed affinity')
    metadata = {**host_metadata(), 'os': 'Linux', 'execution_kind': args.execution_kind,
                'machine_id': args.machine_id, 'isa': platform.machine(), 'cpu_model': cpu_model(),
                'cpus': os.cpu_count(), 'worker_cpus': 1 if client == server else 2,
                'affinity': {'client': client, 'server': server,
                             'client_topology': topology(client), 'server_topology': topology(server)},
                'cpu_governor': read_optional(f'/sys/devices/system/cpu/cpu{client}/cpufreq/scaling_governor'),
                'compiler': version([args.compiler, '--version']), 'compiler_command': command,
                'adapter_sha256': digest(ROOT / 'benchmarks/ipc/linux.cc'), 'binary_sha256': digest(binary), 'transport_framing': '16-byte header plus payload; pipe uses exact header/body reads',
                'clock_implementation': 'syscall(SYS_clock_gettime, CLOCK_MONOTONIC_RAW)',
                'background': 'host services/cgroup scheduling are not controlled by this runner'}
    shutil.copy2(binary, output / 'ipc-linux')
    runs = []
    for index in range(args.runs):
        log = output / f'run-{index}.log'
        execute([str(binary), args.transport, str(client), str(server),
                 *[str(config[field]) for field in ('iterations', 'warmup', 'samples', 'rounds', 'window', 'payload_mask')],
                 str(int(config['include_spin'])), '1'], log, args.timeout)
        runs.append(results.parse_log(log.read_text(), {**config, 'contract_sha256': common_hash}))
    save(output, metadata, runs)


def cmake_cache(build):
    values = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line.split('=', 1)[0]:
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    return values


def run_ame(args):
    import boot
    config = configuration(args)
    results.require(args.placement != 'different_cpu' or args.cpus > 1, 'different_cpu placement needs at least two CPUs')
    for name in ('AME_QEMU_IOMMU', 'AME_QEMU_TESTDEV', 'AME_QEMU_MEM', 'AME_QEMU_GPU_DEVICE'):
        results.require(not os.environ.get(name), f'unset {name} for the fixed benchmark machine')
    output = output_directory(args)
    build = args.build.resolve()
    command = ['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'scripts/build/toolchain-aarch64.cmake'),
               '-DCMAKE_BUILD_TYPE=' + args.build_type, '-DANKO_BOARD=qemu-virt',
               '-DANKO_BUILD_DEV=ON', '-DDANGO_BUILD_LINUX_ARTIFACTS=OFF', '-DAME_IPC_BENCH_SERVER_CPU=0',
               f'-DAME_IPC_BENCH_REFERENCE_FRAMING={int(args.reference_framing)}']
    for key, value in config.items():
        suffix = 'SPIN' if key == 'include_spin' else key.upper()
        command.append(f'-DAME_IPC_BENCH_{suffix}={int(value)}')
    execute(command, output / 'configure.log')
    execute(['cmake', '--build', str(build), '--target', 'anko', 'ipc_bench', '-j', str(args.jobs)],
            output / 'build.log')
    cache = cmake_cache(build)
    results.require(cache['CMAKE_BUILD_TYPE'] == args.build_type and cache['ANKO_BOARD'] == 'qemu-virt',
                    'benchmark build configuration mismatch')
    common_hash = digest(ROOT / 'benchmarks/ipc/workload.h')
    packed = not args.reference_framing and 'kFramePacked' in (ROOT / 'userland/lib/include/mochi/framed_ring.h').read_text()
    metadata = {**host_metadata(), 'os': 'Ame', 'execution_kind': 'qemu-tcg',
                'machine_id': args.machine_id, 'isa': 'aarch64', 'cpu_model': args.cpu,
                'cpus': args.cpus, 'worker_cpus': min(2, args.cpus), 'memory_mib': 512, 'entry_el': 2,
                'qemu_version': version(['qemu-system-aarch64', '--version']),
                'qemu_acceleration': 'TCG default (no KVM); guest timer values are not physical CPU cycles',
                'framing_implementation': 'slotwise_reference' if args.reference_framing else ('mochi_packed' if packed else 'mochi_batched'),
                'framing_max_publication_slots': 1 if args.reference_framing else (128 if packed else 16),
                'ring_library_sha256': digest(ROOT / 'userland/lib/include/mochi/ring.h'),
                'framing_library_sha256': digest(ROOT / 'userland/lib/include/mochi/framed_ring.h'),
                'reference_library_sha256': digest(ROOT / 'benchmarks/ipc/slotwise_reference.h'),
                'adapter_sha256': digest(ROOT / 'benchmarks/ipc/ame.cc'), 'kernel_sha256': digest(build / 'kernel/anko.bin'),
                'build_type': cache['CMAKE_BUILD_TYPE'], 'benchmark_optimization': '-O2',
                'compiler': version([cache.get('AARCH64_GXX') if cache.get('AARCH64_GXX', '').endswith('g++') else cache.get('CLANGXX_BIN', 'clang++'), '--version']),
                'configure_command': command, 'development_kernel': True,
                'ring_order': 7 if packed else 6, 'inline_slot_payload_bytes': 48,
                'transport_framing': 'copied packed records; legacy FIRST/LAST fallback' if packed else 'FIRST/LAST native ring slots; copied reassembly for logical payloads over 48 bytes',
                'tx_backpressure': 'cursor/credit wait in blocking mode; CPU relax in spin mode' if packed else 'yield when full in blocking mode; CPU relax in spin mode',
                'adaptive_poll_max_iterations': 64 if packed else 0,
                'adaptive_poll_placement': 'different_cpu only; zero same_cpu polling',
                'clock_implementation': 'mx_clock_ns syscall; platform-corrected monotonic system counter',
                'background': 'isolated bundle: bootstrap parent parks; two trusted worker processes; ordinary services absent'}
    runs = []
    metadata['placement_builds'] = []
    # Ame affinity can only be set on suspended threads. Use a fresh pair per
    # placement rather than modifying scheduler behaviour to run a benchmark.
    shutil.copy2(build / 'kernel/anko.bin', output / 'anko.bin')
    for server_cpu in range(min(2, args.cpus)):
        if args.placement != 'both' and (server_cpu == 0) != (args.placement == 'same_cpu'):
            continue
        execute(['cmake', '-S', str(ROOT), '-B', str(build),
                 f'-DAME_IPC_BENCH_SERVER_CPU={server_cpu}'], output / f'configure-cpu{server_cpu}.log')
        execute(['cmake', '--build', str(build), '--target', 'anko', 'ipc_bench', '-j', str(args.jobs)],
                output / f'build-cpu{server_cpu}.log')
        shutil.copy2(build / 'ipc-bench.mxb', output / f'cpu{server_cpu}.mxb')
        metadata['placement_builds'].append({'server_cpu': server_cpu,
            'bundle_sha256': digest(build / 'ipc-bench.mxb'),
            'benchmark_elf_sha256': digest(build / 'userland/bench.ipc')})
        for index in range(args.runs):
            destination = output / f'cpu{server_cpu}-run-{index}'
            with contextlib.redirect_stdout(io.StringIO()):
                ok = boot.run(build, 'direct', args.cpus, args.timeout,
                              bundle=build / 'ipc-bench.mxb', markers=['IPC_BENCH_DONE'], output=destination,
                              cpu=args.cpu)
            results.require(ok, f'Ame benchmark failed or timed out; see {destination}')
            runs.append(results.parse_log((destination / 'host.log').read_text(),
                                         {**config, 'contract_sha256': common_hash}))
    save(output, metadata, runs)


def import_logs(args):
    output = output_directory(args)
    runs = [results.parse_log(path.read_text()) for path in args.logs]
    metadata = {'os': args.os, 'execution_kind': args.execution_kind, 'machine_id': args.machine_id,
                'isa': args.isa, 'cpu_model': args.cpu_model, 'cpus': args.cpus,
                'host_identity': args.machine_id, 'metadata_source': 'operator supplied for external capture',
                'source_logs': [str(path.resolve()) for path in args.logs]}
    for index, path in enumerate(args.logs):
        (output / f'run-{index}.log').write_bytes(path.read_bytes())
    save(output, metadata, runs)


def parser():
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = result.add_subparsers(dest='action', required=True)
    for action in ('linux', 'ame'):
        item = sub.add_parser(action)
        item.add_argument('--output', type=Path)
        item.add_argument('--build', type=Path, default=ROOT / ('out/ipc-bench' if action == 'ame' else 'out/ipc-bench-linux'))
        item.add_argument('--runs', type=int, default=3, help='independent process pairs or QEMU boots')
        item.add_argument('--iterations', type=int, default=1024, help='operations per aggregate round')
        item.add_argument('--warmup', type=int, default=256)
        item.add_argument('--samples', type=int, default=4096, help='separate individually timed RTT samples per RPC case')
        item.add_argument('--rounds', type=int, default=5)
        item.add_argument('--window', type=int, default=32, help='one completion ACK per stream window')
        item.add_argument('--payloads', default='0,8,48,256,4096')
        item.add_argument('--include-spin', action='store_true', help='additional different-CPU active polling cases (Ame only)')
        item.add_argument('--timeout', type=float, default=300, help='watchdog seconds per independent run')
        item.add_argument('--machine-id', help='explicit same-machine identifier for cross-OS comparison')
        if action == 'linux':
            item.add_argument('--transport', choices=('seqpacket', 'pipe'), default='seqpacket')
            item.add_argument('--compiler', default='clang++')
            item.add_argument('--client-cpu', type=int)
            item.add_argument('--server-cpu', type=int)
            item.add_argument('--execution-kind', choices=('native', 'qemu-tcg', 'other-vm'), default='native')
        else:
            item.add_argument('--cpu', choices=('cortex-a53', 'cortex-a57', 'max'), default='cortex-a53',
                              help='A53 matches the primary P20 Lite ISA; TCG timings are not physical cycles')
            item.add_argument('--build-type', choices=('Release', 'RelWithDebInfo'), default='Release',
                              help='kernel compiler configuration; Release by default')
            item.add_argument('--reference-framing', action='store_true',
                              help='slot-by-slot transport control with the current wait/wake fix')
            item.add_argument('--placement', choices=('both', 'same_cpu', 'different_cpu'), default='both')
            item.add_argument('--cpus', type=int, choices=(1, 2, 4), default=2)
            item.add_argument('--jobs', type=int, default=4)
    item = sub.add_parser('import', help='validate captured serial logs from native hardware or another adapter')
    item.add_argument('logs', nargs='+', type=Path)
    item.add_argument('--output', type=Path)
    for field in ('os', 'machine-id', 'isa', 'cpu-model'):
        item.add_argument('--' + field, required=True)
    item.add_argument('--execution-kind', choices=('native', 'qemu-tcg', 'other-vm'), required=True)
    item.add_argument('--cpus', type=int, required=True)
    item = sub.add_parser('compare')
    item.add_argument('baseline', type=Path)
    item.add_argument('candidate', type=Path)
    item.add_argument('--cross-os', action='store_true')
    item.add_argument('--output', type=Path)
    return result


def main():
    args = parser().parse_args()
    try:
        if args.action == 'linux': run_linux(args)
        elif args.action == 'ame': run_ame(args)
        elif args.action == 'import': import_logs(args)
        else:
            report = results.compare(json.loads(args.baseline.read_text()),
                                     json.loads(args.candidate.read_text()), args.cross_os)
            text = json.dumps(report, indent=2) + '\n'
            if args.output: args.output.write_text(text)
            else: print(text, end='')
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f'IPC benchmark failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
