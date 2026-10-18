"""Host scheduler regression tests with substituted privileged hardware boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
root = Path(__file__).resolve().parents[1]

def run_probe():
    with tempfile.TemporaryDirectory(prefix='ame-scheduler-') as temporary:
        build_probe(Path(temporary))

def build_probe(work, *, snapshot=None, benchmark=False, run=True, sanitizer='address,undefined',
                probe_source=None, extra_sources=()):
    kernel = (snapshot or root) / 'kernel'
    src = (kernel/'sched/runqueue.cc').read_text()
    # Substitute privileged hardware boundaries only; scheduler branches are unchanged.
    def replace(pattern, replacement):
        nonlocal src
        src, count = re.subn(pattern, lambda _: replacement, src, flags=re.S)
        assert count == 1, (pattern, count)
    replace(r'u64 sched_clock_ns\(\) \{.*?\n\}', 'u64 sched_clock_ns() { return audit_now; }')
    replace(r'static inline void send_event\(\).*?\n', 'static inline void send_event() {}\n')
    replace(r'static inline void irq_enable\(\).*?\n', 'static inline void irq_enable() {}\n')
    replace(r'static inline void irq_disable\(\).*?\n', 'static inline void irq_disable() {}\n')
    replace(r'static inline void wait_for_interrupt\(\).*?\n\}', 'static inline void wait_for_interrupt() { audit_wait(); }')
    replace(r'    asm volatile\("mrs %0, CNTFRQ_EL0" : "=r"\(g_cntfrq\)\);', '    g_cntfrq = 1000000000;')
    replace(r'u32 current_cpu_id\(\) \{.*?\n\}', 'u32 current_cpu_id() { return audit_cpu; }')
    replace(r'static u64 active_ttbr0\(\) \{.*?\n\}', 'static u64 active_ttbr0() { return 0; }')
    (work/'runqueue_host.cc').write_text('extern thread_local unsigned long long audit_now;\nextern thread_local unsigned int audit_cpu;\nextern void audit_wait();\n'+src)
    regression = Path(probe_source) if probe_source else (snapshot or root) / 'tests/scheduler_probe.cc'
    if not regression.exists():
        regression = root / 'tests/scheduler_probe.cc'
    if benchmark and not probe_source:
        source = (root/'tests/scheduler_benchmark.cc').read_text().replace(
            '#include "scheduler_probe.cc"', '#include "'+str(regression)+'"')
        (work/'benchmark.cc').write_text(source)
    cmd = ['clang++', '-std=c++23', '-pthread', '-DANKO_HOST_TEST']
    cmd += ['-O3'] if benchmark else ['-O1', '-g', '-fsanitize='+sanitizer, '-fno-omit-frame-pointer']
    cmd += ['-I'+str(kernel/'include'), '-I'+str(kernel/'sched')]
    cmd += ['-I'+str(root/p) for p in ['kernel/include', 'shared/include']]
    cmd += ['-idirafter', str(root/'lib/include')]
    cmd += [str(work/'benchmark.cc' if benchmark and not probe_source else regression), str(work/'runqueue_host.cc')]
    cmd += [str(kernel/'sched'/p) for p in ['accounting.cc', 'fair_queue.cc', 'sched_class.cc', 'handoff.cc', 'steal.cc', 'sched_context.cc', 'metrics.cc']]
    cmd += [str(kernel/'cap'/p if (kernel/'cap'/p).exists() else root/'kernel/cap'/p)
            for p in ['cap_table.cc', 'object.cc']]
    cmd += [str(p) for p in extra_sources]
    cmd += ['-o', str(work/'probe')]
    subprocess.run(cmd, check=True)
    if run:
        subprocess.run([str(work/'probe')], check=True, timeout=30)
    return work/'probe'


class Scheduler(unittest.TestCase):
    def test_queues_budgets_fairness_and_ownership(self):
        run_probe()

if __name__ == "__main__":
    unittest.main()
