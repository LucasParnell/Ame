from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('clang++'), 'requires clang++')
class NativeVulkan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='ame-native-vulkan-')
        cls.out=Path(cls.temp.name)
        subprocess.run(['python3',str(ROOT/'scripts/vulkan/generate-frontend.py'),
                        '--output',str(cls.out)],check=True,capture_output=True)
        sources=['tests/native_vulkan_probe.cc','userland/libvulkan/client.cc',
                 'userland/libvulkan/memory.cc','linux-artifacts/panfrost-renderer/vulkan_rpc.cc']
        cmd=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
             '-Wno-missing-field-initializers','-fsanitize=address,undefined']
        cmd += ['-I'+str(ROOT/p) for p in ['third_party/vulkan/include','shared/include',
                 'userland/libgraphics/include','userland/libvulkan','userland/libvulkan/include']]
        cmd += ['-I'+str(cls.out)]
        cmd += [str(ROOT/p) for p in sources]
        cmd += [str(cls.out/p) for p in ['vk-rpc-client.cc','vk-rpc-metadata.cc']]
        for source,name in [('renderer.c','triangle.o'),('suite.c','suite.o')]:
            subprocess.run(['clang','-std=c11','-O1','-g','-DAME_NATIVE_VULKAN=1',
                            '-fsanitize=address,undefined','-I'+str(ROOT/'third_party/vulkan/include'),
                            '-I'+str(ROOT/'shared/include'),'-I'+str(ROOT/'userland/libvulkan'),
                            '-c',str(ROOT/'linux-artifacts/panvk-renderer'/source),'-o',str(cls.out/name)],
                           check=True,capture_output=True)
        cmd += ['-DAME_NATIVE_VULKAN=1','-idirafter',str(ROOT/'lib/include'),
                str(ROOT/'userland/libvulkan/native_renderer.cc'),str(ROOT/'userland/libvulkan/present.cc'),
                str(ROOT/'lib/libk/printf.cc'),str(cls.out/'triangle.o'),str(cls.out/'suite.o')]
        cmd += ['-ldl','-o',str(cls.out/'probe')]
        result=subprocess.run(cmd,capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_checked_codec_without_gpu(self):
        result=subprocess.run([str(self.out/'probe'),'--codec-only'],capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    @unittest.skipUnless(os.environ.get('AME_TEST_VULKAN_SOFTWARE_ICD'),
                         'set AME_TEST_VULKAN_SOFTWARE_ICD to an explicit software ICD')
    def test_real_software_vulkan_through_native_frontend(self):
        env=dict(os.environ,VK_DRIVER_FILES=os.environ['AME_TEST_VULKAN_SOFTWARE_ICD'],
                 AME_VK_RPC_LIBRARY='libvulkan.so.1',LP_NUM_THREADS='1',ASAN_OPTIONS='detect_leaks=0')
        result=subprocess.run([str(self.out/'probe')],env=env,capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('panvk suite: 53 ',result.stdout)
        self.assertIn('0 skipped (GPU pixels verified)',result.stdout)
        self.assertNotIn('Validation Error',result.stdout+result.stderr)
        self.assertNotIn('VUID-',result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
