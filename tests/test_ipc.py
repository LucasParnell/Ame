"""Production IPC and scheduler with only privileged hardware boundaries replaced."""
from pathlib import Path
import re
import tempfile
import unittest
from test_scheduler import build_probe, root


def build_ipc(work, *, snapshot=None, benchmark=False, sanitizer='address,undefined', run=True):
    kernel = (snapshot or root) / 'kernel'
    source = (kernel/'ipc/doorbell.cc').read_text()
    # Architecture hint only, including older comparison snapshots. IRQ, lock
    # and publication algorithms below remain the production implementations.
    source = '#include "libk/spin.h"\n' + source.replace(
        'asm volatile("yield" ::: "memory");', 'libk::spin_pause();')
    for pattern, replacement in [
        (r'static inline u64 local_irq_save_disable\(\) \{.*?\n\}',
         'static inline u64 local_irq_save_disable() { return 0; }'),
        (r'static inline void local_irq_restore\(u64 daif\) \{.*?\n\}',
         'static inline void local_irq_restore(u64) {}'),
    ]:
        source, count = re.subn(pattern, replacement, source, flags=re.S)
        assert count == 1
    source = source.replace('#include "ipc_internal.h"',
                            '#include "'+str(kernel/'ipc/ipc_internal.h')+'"')
    (work/'doorbell_host.cc').write_text(source)
    return build_probe(work, snapshot=snapshot, benchmark=benchmark, run=run,
        sanitizer=sanitizer,
        probe_source=root/('tests/ipc_benchmark.cc' if benchmark else 'tests/ipc_probe.cc'),
        extra_sources=[work/'doorbell_host.cc', kernel/'ipc/ring.cc'])


class Ipc(unittest.TestCase):
    def test_rights_lifetimes_deadlines_wakes_and_concurrency(self):
        with tempfile.TemporaryDirectory(prefix='ame-ipc-') as directory:
            build_ipc(Path(directory))


if __name__ == '__main__':
    unittest.main()
