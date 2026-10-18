from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DiskStore(unittest.TestCase):
    def test_scalability_and_every_sector_crash_boundary(self):
        with tempfile.TemporaryDirectory(prefix='ame-fs-store-') as directory:
            work=Path(directory)
            build=work/'build'
            for name in ('kernel/anko','kernel/anko.bin','userland/manifest.v0','manifest.json','system.mxb','userland/test.mxi'):
                p=build/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixture contents\n')
            image=work/'volume.img'
            subprocess.run(['python3',str(ROOT/'scripts/mx/mkamefs.py'),'create','--build-dir',str(build),'--output',str(image),'--size','256M'],check=True,capture_output=True)
            executable=work/'probe'
            result=subprocess.run(['clang++','-std=c++23','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'shared/include'),'-idirafter',str(ROOT/'lib/include'),str(ROOT/'tests/fs_store_probe.cc'),str(ROOT/'userland/fs/store.cc'),'-o',str(executable)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(executable),str(image)],capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout)
            small=work/'full.img'
            subprocess.run(['python3',str(ROOT/'scripts/mx/mkamefs.py'),'create','--build-dir',str(build),'--output',str(small),'--size','32M'],check=True,capture_output=True)
            result=subprocess.run([str(executable),str(small),'full'],capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout)

if __name__=='__main__':unittest.main()
