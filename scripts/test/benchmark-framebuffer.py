#!/usr/bin/env python3
"""Compare the current raster against the pre-raster framebuffer implementation.

Runs on an AArch64 host with SIMD disabled in both raster engines. The harness
uses the host clock and cached host memory; these are not scanout/phone timings.
"""
import argparse
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASELINE = '0d4108bf44f175ea49f2a25014643fddecebe3f2'


def function(source, name):
    start = source.rfind('static ', 0, source.index(name))
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default=BASELINE)
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    if platform.machine().lower() not in ('arm64', 'aarch64'):
        parser.error('this benchmark requires an AArch64 host')
    old = subprocess.check_output([
        'git', 'show', f'{args.baseline_ref}:userland/drivers/framebuffer/main.cc'],
        cwd=ROOT, text=True)
    source = '#include "' + str(ROOT / 'userland/lib/include/mochi/framebuffer_raster.h') + '"\n'
    source += 'constexpr uint32_t kFbWidth = 1080, kFbHeight = 2280;\n'
    source += '\n'.join(function(old, name) for name in
                        ('fb_clear', 'min_u32', 'fb_draw_glyph', 'fb_draw_text'))
    source += '''
extern "C" void old_clear(uint32_t* p, uint32_t c) { fb_clear(p, c); }
extern "C" void new_clear(uint32_t* p, uint32_t c) {
    mochi::framebuffer::Raster{p,kFbWidth,kFbHeight}.clear(c);
}
extern "C" void old_text(uint32_t* p, const FbDrawTextCmd& c) { fb_draw_text(p,c); }
extern "C" void new_text(uint32_t* p, const FbDrawTextCmd& c) {
    mochi::framebuffer::Raster{p,kFbWidth,kFbHeight}.draw_text(c);
}
'''
    with tempfile.TemporaryDirectory(prefix='ame-framebuffer-benchmark-') as directory:
        work = Path(directory)
        (work / 'engines.cc').write_text(source)
        subprocess.run([
            args.cxx, '-std=c++23', '-O3', '-ffreestanding', '-mgeneral-regs-only',
            '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
            '-c', str(work / 'engines.cc'), '-o', str(work / 'engines.o')], check=True)
        subprocess.run([
            args.cxx, '-std=c++23', '-O3', '-I' + str(ROOT / 'shared/include'),
            str(ROOT / 'tests/framebuffer_benchmark.cc'), str(work / 'engines.o'),
            '-o', str(work / 'bench')], check=True)
        print(f'cached host memory; SIMD disabled; baseline={args.baseline_ref}', flush=True)
        subprocess.run([str(work / 'bench')], check=True, timeout=120)


if __name__ == '__main__':
    main()
