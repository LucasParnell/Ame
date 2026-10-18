#!/usr/bin/env python3
"""Fetch and hash-check the Huawei OSS sources used by the RPMB host oracles."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

REVISION = '65e545cbaa11d132e243287b150146bc2a51c45e'
BASE = f'https://raw.githubusercontent.com/hisi-oss/android_kernel_huawei_hi6250-8/{REVISION}/'
FILES = {
    'hisi_rpmb.c': ('drivers/mmc/card/hisi_rpmb.c',
                   'd8b26ecc8a8534ecb112aaf51b9d184067e1ea3294076f620cae0b9d50e65ade'),
    'hisi_rpmb.h': ('drivers/mmc/card/hisi_rpmb.h',
                   '85435c172a33ff32eea2d3c95d1bcab80db639c14f7422a10da84041671e53b8'),
    'include_linux_mmc_rpmb.h': ('include/linux/mmc/rpmb.h',
                               '9ca18286c3286127571ea485ea33b960f05c05a2518b6a3c999e46dce7f9134f'),
    'emmc-rpmb.c': ('drivers/mmc/core/emmc-rpmb.c',
                    'df56965d38580067c4863ac9921e6ed17ed02e81abaa9d4b911daf7f77399d08'),
    'emmc-rpmb.h': ('drivers/mmc/core/emmc-rpmb.h',
                    'c2ad6b6d439aced6ea62a85b271a1c218cfc5bbbe594b61cad9c698b327d5877'),
    'bootdevice.h': ('include/linux/bootdevice.h',
                     'ba20eae7c010b95cd22a3e7757796d69d5c5b0c69e6ae7b6b0e121907ccd8e53'),
    'global_ddr_map.h': ('drivers/hisi/ap/platform/hi6250/global_ddr_map.h',
                         '653d6f0898103646057fe5eb0c817baa73facdc493b5249313bd7016ed59377b'),
    'share.dtsi': ('arch/arm64/boot/dts/auto-generate/config/hi6250/hisi_6250_bl31_kernel_share_mem.dtsi',
                   '9dc4f50b3386c2ec628fa9167f2cda3a4229149b26f886138af00b6202add43b'),
    'merge_hi6250_defconfig': ('arch/arm64/configs/merge_hi6250_defconfig',
                               '10921aba1843762a557f46538ee5048a451dd1140d60b96a620362374bb7ff14'),
    'arch_arm64_kernel_smp.c': ('arch/arm64/kernel/smp.c',
                                '0f15705fb38ffd3ce1ce3a11e8179f654506d8d0e9fe4772abc67f3f89cfefb9'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--offline', action='store_true', help='verify existing sources without downloading')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, (source, digest) in FILES.items():
        target = args.output / name
        data = target.read_bytes() if target.exists() else b''
        if hashlib.sha256(data).hexdigest() != digest:
            if args.offline:
                parser.exit(1, f'{target}: missing or SHA-256 mismatch\n')
            data = urllib.request.urlopen(BASE + source, timeout=30).read()
            if hashlib.sha256(data).hexdigest() != digest:
                parser.exit(1, f'{source}: downloaded SHA-256 mismatch; existing file preserved\n')
            target.write_bytes(data)
        print(f'verified {name}')


if __name__ == '__main__':
    main()
