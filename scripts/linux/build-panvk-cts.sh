#!/usr/bin/env bash
# Build pinned Khronos Vulkan CTS for the AArch64 test guest.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
CTS="${DANGO_PANVK_CTS_SOURCE:-${AME}/out/vulkan-cts-midgard}"
OUT="${DANGO_PANVK_CTS_BUILD:-${AME}/out/vulkan-cts-midgard-build}"
REV=7aa3551652743f11e77cc28dd8c19c9f872ecf79
if [[ ! -d "${CTS}/.git" ]]; then
    git clone --depth 1 --branch vulkan-cts-1.3.10.0 \
        https://github.com/KhronosGroup/VK-GL-CTS.git "${CTS}"
fi
[[ "$(git -C "${CTS}" rev-parse HEAD)" == "$REV" ]] || {
    echo 'CTS source does not match the pinned revision' >&2; exit 1;
}
(cd "${CTS}" && python3 external/fetch_sources.py)
AMBER_PATCH="${AME}/third_party/cts/0001-amber-explicit-integer-header.patch"
AMBER="${CTS}/external/amber/src"
if patch --dry-run --batch --forward --silent -p1 -d "${AMBER}" < "${AMBER_PATCH}" >/dev/null 2>&1; then
    patch --batch --forward --silent -p1 -d "${AMBER}" < "${AMBER_PATCH}"
elif ! patch --dry-run --batch --reverse --silent -p1 -d "${AMBER}" < "${AMBER_PATCH}" >/dev/null 2>&1; then
    echo 'Amber source does not match the pinned compatibility patch' >&2; exit 1
fi
mkdir -p "${OUT}"
cat > "${OUT}/aarch64-toolchain.cmake" <<'CMAKE'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE
# VVL's dependency helper forces --parallel cpu_count(), ignoring the outer
# Ninja job setting. Limit those nested builds without changing CTS sources.
mkdir -p "${OUT}/job-limit"
python3 - "${OUT}/job-limit/cmake" <<'PY_CMAKE_LIMIT'
from pathlib import Path
import shutil,sys
real=shutil.which('cmake')
p=Path(sys.argv[1])
p.write_text('#!/usr/bin/env python3\nimport os,sys\n'
    'args=sys.argv[1:]\n'
    'if "--build" in args:\n'
    '    for i,arg in enumerate(args):\n'
    '        if arg=="--parallel" and i+1<len(args) and args[i+1].isdigit():\n'
    '            args[i+1]=os.environ.get("DANGO_CTS_JOBS","1")\n'
    f'os.execv({real!r},[{real!r}]+args)\n')
p.chmod(0o755)
PY_CMAKE_LIMIT
export PATH="${OUT}/job-limit:${PATH}"
export CMAKE_BUILD_PARALLEL_LEVEL="${DANGO_CTS_JOBS:-1}"
cmake -S "${CTS}" -B "${OUT}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${OUT}/aarch64-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=vulkan_headless -DDE_CPU=DE_CPU_ARM_64
cmake --build "${OUT}" --target deqp-vk -j"${DANGO_CTS_JOBS:-1}"
python3 - "${CTS}" "${OUT}" "$REV" "${AMBER_PATCH}" <<'PY'
from pathlib import Path
import hashlib,json,subprocess,sys
source,out,rev=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
binary=out/'external/vulkancts/modules/vulkan/deqp-vk'
deps={str(p.parent.relative_to(source)):subprocess.check_output(['git','-C',str(p.parent),'rev-parse','HEAD'],text=True).strip()
      for p in source.glob('external/*/src/.git')}
(out/'provenance.json').write_text(json.dumps({'repository':'https://github.com/KhronosGroup/VK-GL-CTS.git',
    'revision':rev,'tag':'vulkan-cts-1.3.10.0','dependencies':deps,
    'target_cpu':'DE_CPU_ARM_64','jobs':1,
    'dependency_compatibility_patch_sha256':hashlib.sha256(Path(sys.argv[4]).read_bytes()).hexdigest(),
    'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
    'hardware_execution':False},indent=2)+'\n')
PY
echo "CTS binary: ${OUT}/external/vulkancts/modules/vulkan/deqp-vk"
