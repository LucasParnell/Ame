#!/usr/bin/env python3
"""Compare real IPC hot/cold host workloads against captured source snapshots.

Privileged IRQ/clock/frame-entry boundaries use the regression fixtures. Both
versions see the same 2,048 unrelated lifetime records. Concurrent throughput
is wall time divided by all operations, not one CPU's latency or phone timing.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from test_ipc import build_ipc


def build_ring(work,snapshot):
    binary=work/'ring'
    includes=[ROOT/'tests/mochi_host',(snapshot or ROOT)/'userland/lib/include',
              ROOT/'userland/lib/include',ROOT/'shared/include']
    subprocess.run(['clang++','-std=c++23','-O3','-pthread',*['-I'+str(p) for p in includes],
                    '-idirafter',str(ROOT/'lib/include'),str(ROOT/'tests/ring_benchmark.cc'),
                    '-o',str(binary)],check=True)
    return binary


def measure(work,snapshot,runs):
    work.mkdir()
    ipc=build_ipc(work,snapshot=snapshot,benchmark=True,run=False)
    ring=build_ring(work,snapshot)
    samples={}
    for _ in range(runs):
        for binary in [ipc,ring]:
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=30)
            for line in result.stdout.splitlines():
                if not line.startswith('case='): continue
                row=dict(field.split('=') for field in line.split())
                samples.setdefault(row['case'],[]).append(float(row['ns']))
    return {name:{'samples_ns':values,'median_ns':statistics.median(values)}
            for name,values in samples.items()}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-dir',required=True,type=Path)
    parser.add_argument('--runs',type=int,default=7)
    args=parser.parse_args()
    if args.runs<1: parser.error('--runs must be positive')
    with tempfile.TemporaryDirectory(prefix='ame-ipc-benchmark-') as directory:
        work=Path(directory)
        baseline=measure(work/'baseline',args.baseline_dir.resolve(),args.runs)
        current=measure(work/'current',None,args.runs)
        print(json.dumps({'kind':'host IPC hot/cold stress; no phone prediction',
            'baseline':baseline,'current':current,
            'speedup':{name:baseline[name]['median_ns']/row['median_ns']
                       for name,row in current.items()}},indent=2))


if __name__=='__main__': main()
