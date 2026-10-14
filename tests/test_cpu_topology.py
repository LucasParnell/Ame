import pathlib
import shutil
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('dtc') and shutil.which('clang++'), 'requires dtc and clang++')
class CpuTopology(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='ame-cpu-topology-')
        cls.dir = pathlib.Path(cls.work.name); cls.probe = cls.dir / 'probe'
        subprocess.run(['clang++', '-std=c++23', '-O1', '-fsanitize=address,undefined',
                        '-DANKO_BOARD_HUAWEI_ANNE=1',
                        *['-I' + str(ROOT / p) for p in ['kernel/include','lib/include','shared/include']],
                        str(ROOT/'tests/cpu_topology_probe.cc'), str(ROOT/'kernel/platform/cpu_topology.cc'),
                        str(ROOT/'kernel/init/fdt.cc'), str(ROOT/'lib/libk/string.cc'), '-o', str(cls.probe)],
                       check=True, capture_output=True, text=True)
    @classmethod
    def tearDownClass(cls): cls.work.cleanup()

    def topology(self, nodes, boot=0, conduit='smc', cells=2, compatible='"arm,psci"', power=''):
        cpu_nodes = ''.join(f'cpu@{reg:x} {{ device_type="cpu"; reg=<{"0 " if cells == 2 else ""}{reg}>; enable-method="psci"; {extra} }};' for reg, extra in nodes)
        dts = f'/dts-v1/; / {{ cpus {{ #address-cells=<{cells}>; #size-cells=<0>; {cpu_nodes} }}; psci {{ compatible={compatible}; method="{conduit}"; cpu_on=<0xc4000003>; {power} }}; }};'
        (self.dir/'input.dts').write_text(dts)
        subprocess.run(['dtc','-I','dts','-O','dtb','-o',str(self.dir/'input.dtb'),str(self.dir/'input.dts')],check=True,capture_output=True)
        r = subprocess.run([str(self.probe),str(self.dir/'input.dtb'),hex(boot)],check=True,capture_output=True,text=True)
        fields = r.stdout.split()
        self.power_ids = tuple(int(x, 16) for x in r.stderr.split())
        return tuple(int(x) for x in fields[:3]), int(fields[3],16), [int(x,16) for x in fields[4:]]

    def test_huawei_two_clusters_and_dtb_order(self):
        meta, fn, ids = self.topology([(x,'') for x in [0,1,2,3,0x102,0x103,0x100,0x101]])
        self.assertEqual(meta,(8,1,0)); self.assertEqual(fn,0xc4000003)
        self.assertEqual(ids,[0,1,2,3,0x100,0x101,0x102,0x103])
    def test_boot_core_is_logical_zero(self):
        meta,_,ids = self.topology([(x,'') for x in [0,1,2,3,0x100,0x101,0x102,0x103]],boot=0x80000102)
        self.assertEqual(meta[0],8); self.assertEqual(ids,[0x102,0,1,2,3,0x100,0x101,0x103])
    def test_disabled_cpu_and_one_cell_reg(self):
        meta,_,ids = self.topology([(0,''),(1,'status="disabled";'),(2,'')],conduit='hvc',cells=1)
        self.assertEqual(meta,(2,1,1)); self.assertEqual(ids,[0,2])
    def test_invalid_conduit_is_not_called(self):
        meta,_,ids = self.topology([(0,''),(1,'')],conduit='invalid')
        self.assertEqual(meta,(2,0,0)); self.assertEqual(ids,[0,1])

    def test_legacy_cpu_on_does_not_imply_system_power(self):
        self.topology([(0, '')])
        self.assertEqual(self.power_ids, (0, 0))

    def test_explicit_legacy_power_ids_from_phone(self):
        self.topology([(0, '')], power='system_off=<0x84000008>; system_reset=<0x84000009>;')
        self.assertEqual(self.power_ids, (0x84000008, 0x84000009))

    def test_modern_psci_supplies_standard_power_ids(self):
        for compat in ('"arm,psci-0.2", "arm,psci"', '"arm,psci-1.0"'):
            self.topology([(0, '')], compatible=compat)
            self.assertEqual(self.power_ids, (0x84000008, 0x84000009))

    def test_invalid_power_ids_and_conduit_are_not_called(self):
        self.topology([(0, '')], compatible='"arm,psci-0.2"',
                      power='system_off=<0x12345678>; system_reset=<0>;')
        self.assertEqual(self.power_ids, (0, 0))
        self.topology([(0, '')], conduit='invalid', compatible='"arm,psci-0.2"')
        self.assertEqual(self.power_ids, (0, 0))

if __name__ == '__main__': unittest.main()
