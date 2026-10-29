#!/usr/bin/env python3
"""Profile generation, validation, copies and framing separately on the host.

The app target bench.ipc.components runs the same probe on Ame. These are
component costs, not two-process IPC timings or cross-OS comparison data.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--iterations', type=int, default=8192)
    args = parser.parse_args()
    if not 16 <= args.iterations <= 1048576:
        parser.error('iterations must be in 16..1048576')
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.output.resolve() / 'probe'
    command = ['clang++', '-std=c++23', '-O2', '-DAME_IPC_COMPONENT_HOST',
               f'-DAME_IPC_COMPONENT_ITERATIONS={args.iterations}',
               '-I' + str(ROOT / 'tests/mochi_host'), '-I' + str(ROOT / 'userland/lib/include'),
               '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
               str(ROOT / 'benchmarks/ipc/components.cc'), str(ROOT / 'lib/libk/printf.cc'),
               '-o', str(binary)]
    subprocess.run(command, check=True)
    log = subprocess.check_output([str(binary)], text=True, timeout=60)
    (args.output / 'raw.log').write_text(log)
    assert log.splitlines().count('IPC_COMPONENT_DONE') == 1
    rows = [json.loads(line.removeprefix('IPC_COMPONENT ')) for line in log.splitlines()
            if line.startswith('IPC_COMPONENT ')]
    assert len(rows) == 20 and all(row['elapsed_ns'] > 0 for row in rows)
    checksum = 0
    for row in rows:
        if row['component'] in ('validate', 'framed_copy_roundtrip'):
            checksum += args.iterations + 64
        assert row['checksum'] == checksum, 'component validation/framing failed'
    report = {'kind': 'host_component_diagnostic', 'compile_command': command, 'rows': rows}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    for row in rows:
        print(f"{row['component']:24} {row['payload_bytes']:4} B {row['elapsed_ns'] / row['iterations']:10.2f} ns/op")


if __name__ == '__main__':
    main()
