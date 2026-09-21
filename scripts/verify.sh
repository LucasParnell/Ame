#!/usr/bin/env bash
#
# scripts/verify.sh — green-baseline gate for Ame.
#
# Proves a checkout is healthy with the smallest set of checks that matters:
#
#   configure → build → host regressions → direct boot → kernel lifecycle matrix
#
# The checks are declared as data in CHECKS; the loop below is the only control
# flow. Every check is a shell function that returns non-zero on failure, so the
# gate composes and reads top-to-bottom. Runs on macOS and Linux.
#
# Usage:
#   scripts/verify.sh                                  # out/verify, CPUs 1 2 4
#   AME_VERIFY_BUILD=out/qemu-dev scripts/verify.sh    # reuse an existing tree
#   AME_VERIFY_CPUS="2" scripts/verify.sh              # single-CPU matrix
#   AME_VERIFY_IOMMU=1 scripts/verify.sh               # + Phase C SMMUv3 row
#   AME_VERIFY_GPU=1 scripts/verify.sh                 # + Phase E Linux virtio-gpu
#   AME_VERIFY_CHANNEL=1 scripts/verify.sh             # + Phase G channel row
#   AME_VERIFY_KMS=1 scripts/verify.sh                 # + Phase F KMS display row
#   AME_VERIFY_GPUD=1 scripts/verify.sh                # + Phase H/I GPU control row
#   AME_VERIFY_PANIC=1 scripts/verify.sh                # + panic screen capture
#
# Exit status is 0 only when every check passes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck source=scripts/host.sh
source "${ROOT}/scripts/host.sh"
ame_host_tools

BUILD_DIR="${AME_VERIFY_BUILD:-${ROOT}/out/verify}"
CPUS="${AME_VERIFY_CPUS:-1 2 4}"
BOOT_TIMEOUT="${AME_VERIFY_BOOT_TIMEOUT:-120}"

bold() { printf '\n\033[1;34m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }
fail() { printf '\n\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

# Print the exact toolchain that this gate validated. Kept explicit so a build
# that passes here can be reproduced (and so toolchain drift is visible).
record_toolchain() {
    bold "toolchain"
    note "$(clang --version | head -1)"
    note "$(cmake --version | head -1)"
    note "$(qemu-system-aarch64 --version | head -1)"
}

do_configure() {
    local args=(-S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
                -DANKO_BOARD=qemu-virt -DANKO_BUILD_DEV=ON
                -DDANGO_BUILD_LINUX_ARTIFACTS=OFF)
    # CMake only honours the toolchain on the first configure; passing it again
    # just emits an unused-variable warning.
    [[ -f "$BUILD_DIR/CMakeCache.txt" ]] \
        || args+=(-DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake)
    cmake "${args[@]}" >/dev/null
}

do_build() {
    cmake --build "$BUILD_DIR" --target anko mochi_system >/dev/null
}

do_host_tests() {
    ctest --test-dir "$BUILD_DIR" --output-on-failure >/dev/null
}

# Provenance: the build must record revision, config, signing id, bundle hash.
do_provenance() {
    cmake --build "$BUILD_DIR" --target provenance >/dev/null
    local json="$BUILD_DIR/generated/provenance.json"
    [[ -f "$json" ]] || fail "generated/provenance.json missing"
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("   -> {} {} {} key={} mxb={}".format(d["git"]["describe"], d["build"]["type"], d["build"]["board"], d["signing"]["key_id"], d["artifact"]["sha256"][:12]))' "$json"
}

# boot <marker> <cpus> [bundle] — run one bounded QEMU boot and report a
# one-line summary; the exit status is boot.py's, so the gate can rely on it.
run_boot() {
    local marker="$1" cpus="$2" bundle="${3:-}"
    python3 scripts/test/boot.py --build "$BUILD_DIR" --mode direct \
        --cpus "$cpus" --timeout "$BOOT_TIMEOUT" \
        ${bundle:+--bundle "$bundle"} --marker "$marker" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("   -> {} ({:.2f}s)".format(d["reason"], d["elapsed"])); sys.exit(0 if d["success"] else 1)'
}

do_boot() {
    run_boot 'all services ready' 2
}

do_lifecycle() {
    cmake --build "$BUILD_DIR" --target kernel_regressions >/dev/null
    local c
    for c in $CPUS; do
        note "cpus=$c"
        run_boot 'lifecycle regression passed' "$c" "$BUILD_DIR/lifecycle.mxb"
    done
}

# Supervision: a service is killed and init must restart it (and its dependents).
do_supervision() {
    cmake --build "$BUILD_DIR" --target kernel_regressions >/dev/null
    run_boot 'supervision regression passed' 2 "$BUILD_DIR/restart.mxb"
}

# Durability: write /data/persist.txt and FS_SYNC in one boot, reboot against the
# same (disposable, copied) disk image, and read the file back byte-for-byte.
do_durable() {
    cmake --build "$BUILD_DIR" --target durable_bundle >/dev/null
    python3 scripts/test/durable-persistence.py --build "$BUILD_DIR" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("   -> write={} read={} ({:.2f}s)".format(d["write_phase"], d["read_phase"], d["elapsed"])); sys.exit(0 if d["success"] else 1)'
}

# Phase C: SMMUv3 hostile-DMA verification (spec §8.14).  Opt-in because it
# needs the iommu-testdev reference device and an SMMU-backed topology; the
# default topology (and therefore this gate's byte-for-byte behaviour) is
# unchanged unless AME_VERIFY_IOMMU=1 is set.
do_iommu_dma() {
    python3 scripts/test/iommu_dma.py --build "$BUILD_DIR" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); c=d.get("counters", {}); print("   -> {} faults={} quarantines={} ({:.2f}s)".format(d["reason"], c.get("faults"), c.get("quarantines"), d.get("elapsed") or 0)); sys.exit(0 if d["success"] else 1)'
}

# Phase D: PCI device assignment + interrupt virtualization (spec §6, §7,
# §8.11/§8.12).  Opt-in for the same reason as Phase C: it needs the
# iommu-testdev reference device and an SMMU-backed topology.  The default gate
# is unchanged unless AME_VERIFY_ASSIGN=1 is set.
do_assign() {
    python3 scripts/test/assignment.py --build "$BUILD_DIR" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); c=d.get("counters", {}); print("   -> {} active={} bar={} routes={} injected={} deny={} ({:.2f}s)".format(d["reason"], c.get("active"), c.get("bar_mapped"), c.get("irq_routes"), c.get("irq_injected"), c.get("mediated_deny"), d.get("elapsed") or 0)); sys.exit(0 if d["success"] else 1)'
}

# Linux GPU artifacts are built separately (a Linux container on macOS).
# Keep this build separate so the default hello/no-payload gate stays intact.
# Both GPU rows (Phase E and Phase F) share the same appliance tree.
build_gpu_tree() {
    local gpu_build="${AME_VERIFY_GPU_BUILD:-${ROOT}/out/qemu-gpu}"
    local artifacts="${AME_VERIFY_GPU_ARTIFACTS:-${ROOT}/build/dango-linux-gpu}"
    local args=(-S . -B "$gpu_build" -G Ninja -DCMAKE_BUILD_TYPE=Debug
                -DANKO_BOARD=qemu-virt -DANKO_BUILD_DEV=ON
                -DDANGO_BUILD_LINUX_ARTIFACTS=OFF -DDANGO_LINUX_PROFILE=gpu
                -DDANGO_LINUX_ARTIFACT_DIR="$artifacts")
    [[ -f "$gpu_build/CMakeCache.txt" ]] || args+=(-DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake)
    cmake "${args[@]}" >/dev/null
    cmake --build "$gpu_build" --target anko mochi_system >/dev/null
    printf '%s\n' "$gpu_build"
}

# Resolve the shared GPU tree without hiding a build failure: `--build
# "$(build_gpu_tree)"` would swallow the substitution's exit status and run the
# row with an empty path, reporting a build break as a boot failure.
gpu_tree() {
    local tree
    tree="$(build_gpu_tree)" || fail "gpu build tree"
    [[ -d "$tree" ]] || fail "gpu build tree"
    printf '%s\n' "$tree"
}

do_gpu() {
    local tree
    tree="$(gpu_tree)" || fail "gpu build tree"
    python3 scripts/test/gpu-appliance.py --build "$tree" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("   -> GPU {} {}".format(d["reason"], d["counters"])); sys.exit(0 if d["success"] else 1)'
}

# Phase F: a standalone Linux KMS modeset asserted from a host screendump of the
# assigned virtio-gpu console (no Midix GPU protocol involved).  Opt-in like the
# Phase E row; the default gate is unchanged unless AME_VERIFY_KMS=1 is set.
do_kms() {
    local tree
    tree="$(gpu_tree)" || fail "gpu build tree"
    python3 scripts/test/kms-display.py --build "$tree" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("   -> KMS {} display={} {}".format(d["reason"], d.get("pattern", {}).get("size"), d["counters"])); sys.exit(0 if d["success"] else 1)'
}

# Phase H/I: the bootstrap host GPU control plane (spec §13) and present path,
# asserted from the host.  Opt-in like the Phase E/F rows, which it shares the
# appliance tree with; the default gate is unchanged unless AME_VERIFY_GPUD=1.
do_gpud() {
    local tree
    tree="$(gpu_tree)" || fail "gpu build tree"
    python3 scripts/test/gpud.py --build "$tree" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("   -> GPUD {} display={} {}".format(d["reason"], d.get("pattern", {}).get("size"), d["counters"])); sys.exit(0 if d["success"] else 1)'
}

# Panic screen: a dev-only kernel (ANKO_PANIC_SCREEN_DEMO) panics as soon as the
# framebuffer is bound; the host captures the screen and asserts the logo art and
# headline.  Opt-in, and it builds its own kernel.
do_panic() {
    python3 scripts/test/panic-screen.py --build "${AME_VERIFY_PANIC_BUILD:-${ROOT}/out/qemu-panic}" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); c=d.get("counts", {}); print("   -> panic {} frame={} painted={}".format(d["reason"], c.get("size"), c.get("painted"))); sys.exit(0 if d["success"] else 1)'
}

do_channel() {
    local tree
    tree="$(gpu_tree)" || fail "gpu build tree"
    python3 scripts/test/channel.py --build "$tree" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("   -> channel {} {}".format(d["reason"], d["counters"])); sys.exit(0 if d["success"] else 1)'
}

# Ordered checks: "name:function". Add a row to extend the gate; do not add
# bespoke control flow below.
CHECKS=(
    "configure:do_configure"
    "build:do_build"
    "host-regressions:do_host_tests"
    "provenance:do_provenance"
    "boot-direct:do_boot"
    "lifecycle-matrix:do_lifecycle"
    "supervision-restart:do_supervision"
    "durable-persistence:do_durable"
)

# Opt-in Phase C row.  Absent by default so the default gate is unchanged.
if [[ "${AME_VERIFY_IOMMU:-0}" == "1" ]]; then
    CHECKS+=("iommu-dma:do_iommu_dma")
fi

# Opt-in Phase D row.  Absent by default so the default gate is unchanged.
if [[ "${AME_VERIFY_ASSIGN:-0}" == "1" ]]; then
    CHECKS+=("assignment:do_assign")
fi

if [[ "${AME_VERIFY_GPU:-0}" == "1" ]]; then
    CHECKS+=("gpu-appliance:do_gpu")
fi

# Opt-in Phase F row.
if [[ "${AME_VERIFY_KMS:-0}" == "1" ]]; then
    CHECKS+=("kms-display:do_kms")
fi

# Opt-in Phase H/I row.
if [[ "${AME_VERIFY_GPUD:-0}" == "1" ]]; then
    CHECKS+=("gpud:do_gpud")
fi

# Opt-in panic-screen row.
if [[ "${AME_VERIFY_PANIC:-0}" == "1" ]]; then
    CHECKS+=("panic-screen:do_panic")
fi

if [[ "${AME_VERIFY_CHANNEL:-0}" == "1" ]]; then
    CHECKS+=("dango-channel:do_channel")
fi

record_toolchain
for check in "${CHECKS[@]}"; do
    name="${check%%:*}"
    fn="${check#*:}"
    bold "check: $name"
    "$fn" || fail "$name"
done
bold "verify: all checks passed"
