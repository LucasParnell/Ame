from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('dtc'), 'requires dtc')
class PowerSubsystem(unittest.TestCase):
    def test_accounting_domains_constraints_telemetry_and_staleness(self):
        with tempfile.TemporaryDirectory(prefix='ame-power-core-') as work:
            work = Path(work)
            source = work / 'power.dts'
            source.write_text('''/dts-v1/;
/ { cpus { #address-cells=<1>; #size-cells=<0>;
 cpu@0 { reg=<0>; clocks=<&clock>; operating-points=<400000 700000 1000000 800000 2000000 1000000>; };
 cpu@1 { reg=<1>; clocks=<&clock>; operating-points=<400000 700000 1000000 800000 2000000 1000000>; };
}; clock: clock { #clock-cells=<0>; compatible="hisilicon,hi3xxx-xfreq-clk"; hisilicon,hi3xxx-xfreq-devid=<0>; hisilicon,hi3xxx-xfreq-scbakdata=<0x41c>; hisilicon,set-rate-ipc-cmd=<0x1030a 0>; }; };''')
            subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(work/'power.dtb'), str(source)], check=True, capture_output=True)
            command = ['clang++', '-std=c++23', '-pthread', '-DANKO_HOST_TEST', '-O1', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            command += ['-I'+str(ROOT/p) for p in ('kernel/include', 'shared/include')]
            command += ['-idirafter', str(ROOT/'lib/include'), str(ROOT/'tests/power_subsystem_probe.cc')]
            command += [str(ROOT/'kernel'/p) for p in ('power/accounting.cc', 'power/frequency.cc', 'power/telemetry.cc', 'power/thermal.cc', 'init/fdt.cc')]
            command += [str(ROOT/'lib/libk/string.cc')]
            command += ['-o', str(work/'probe')]
            subprocess.run(command, check=True)
            subprocess.run([str(work/'probe'), str(work/'power.dtb')], check=True, timeout=30)

    def test_nonblocking_mailbox_ack_readback_ownership_and_timeout(self):
        with tempfile.TemporaryDirectory(prefix='ame-dvfs-') as work:
            binary = str(Path(work)/'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-idirafter', str(ROOT/'lib/include'),
                            str(ROOT/'tests/dvfs_mailbox_probe.cc'), '-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=10)

    @unittest.skipUnless((ROOT/'out/anne-device-private/recovery-inspection/observations/firmware.dts').exists(),
                         'requires the private captured Anne DT')
    def test_captured_anne_domains_and_boot_cpu_remapping(self):
        with tempfile.TemporaryDirectory(prefix='ame-power-discovery-') as work:
            work = Path(work)
            subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(work/'phone.dtb'),
                            str(ROOT/'out/anne-device-private/recovery-inspection/observations/firmware.dts')],
                           check=True, capture_output=True)
            command = ['clang++', '-std=c++23', '-O1', '-fsanitize=address,undefined', '-DANKO_HOST_TEST',
                       '-DANKO_BOARD_HUAWEI_ANNE', '-DANKO_ANNE_DIAG_TEST']
            command += ['-I'+str(ROOT/p) for p in ('kernel/include', 'shared/include')]
            command += ['-idirafter', str(ROOT/'lib/include')]
            command += [str(ROOT/p) for p in ('tests/power_discovery_probe.cc', 'kernel/power/frequency.cc',
                        'kernel/power/accounting.cc', 'kernel/platform/cpu_topology.cc',
                        'kernel/init/fdt.cc', 'lib/libk/string.cc')]
            command += ['-o', str(work/'probe')]
            subprocess.run(command, check=True)
            subprocess.run([str(work/'probe'), str(work/'phone.dtb')], check=True, timeout=30)
