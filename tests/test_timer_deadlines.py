"""Production timer selection with only counter/register boundaries replaced."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TimerDeadlines(unittest.TestCase):
    def test_earliest_deadline_cancellation_cpu_ownership_and_maintenance(self):
        source = (ROOT / 'kernel/drivers/timer/generic_timer.cc').read_text()
        source = ('extern unsigned long long audit_ticks();\n'
                  'extern unsigned int audit_frequency, audit_timer_writes;\n'
                  'extern unsigned long long audit_programmed;\n') + source
        source = source.replace('libk::cpu::counter_ticks()', 'audit_ticks()')
        source = source.replace('asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));', 'freq = audit_frequency;')
        source = source.replace('asm volatile("msr CNTP_CVAL_EL0, %0" : : "r"(next) : "memory");',
                                'audit_programmed = next; ++audit_timer_writes;')
        source = source.replace('asm volatile("msr CNTP_CTL_EL0, %0" : : "r"(ctl) : "memory");', '(void)ctl;')
        source = source.replace('asm volatile("isb" ::: "memory");', '')
        with tempfile.TemporaryDirectory(prefix='ame-timer-') as directory:
            work = Path(directory)
            (work / 'timer.cc').write_text(source)
            binary = work / 'probe'
            subprocess.run(['clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                            '-idirafter', str(ROOT / 'lib/include'),
                            str(ROOT / 'tests/timer_deadline_probe.cc'), str(work / 'timer.cc'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
