#!/usr/bin/env bash
# gen-dango-binding.sh — emit the DTS substitutions for the Dango guest binding.
#
# The guest platform numbers (channel window, GPU ECAM/MMIO apertures, IRQ
# numbers) are owned by exactly one file: the guest ABI header
# shared/include/midix/abi/dango_channel_guest.h, which the Linux driver, the
# manager and the device tree all have to agree on. This script reads the macro
# definitions out of that header and prints a sed program that rewrites the
# matching placeholders in the .dts.in templates, so a value can never be
# retyped (and silently drift) in the device tree.
#
# Usage:
#   scripts/linux/gen-dango-binding.sh > dango-binding.sed
#   sed -e "s/@BOOTARGS@/.../" -f dango-binding.sed dango-linux-gpu.dts.in > out.dts
#
# Environment:
#   DANGO_CHANNEL_GUEST_HEADER  override the header path (tests).
#
# Exits non-zero when a required macro is missing, empty, or not an integer
# literal, and when the derived SPI number is inconsistent with the header.

set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
HEADER="${DANGO_CHANNEL_GUEST_HEADER:-${AME}/shared/include/midix/abi/dango_channel_guest.h}"

if [[ ! -f "${HEADER}" ]]; then
    echo "error: guest binding header not found: ${HEADER}" >&2
    exit 1
fi

# read_macro NAME -> the first token after the macro name, e.g. "0x48000000ULL".
read_macro() {
    sed -n -e "s/^#[[:space:]]*define[[:space:]][[:space:]]*$1[[:space:]][[:space:]]*\([^[:space:]/]*\).*/\1/p" \
        "${HEADER}" | head -n 1
}

# macro_dec NAME -> the macro value as a decimal integer.
macro_dec() {
    local name="$1" raw value
    raw="$(read_macro "${name}")"
    value="${raw%[uUlL]}"
    value="${value%[uUlL]}"
    value="${value%[uUlL]}"
    if [[ -z "${value}" ]] || ! [[ "${value}" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
        echo "error: ${name} is missing or not an integer literal in ${HEADER} (got '${raw}')" >&2
        exit 1
    fi
    printf '%d' "$(( value ))"
}

# emit PLACEHOLDER FORMAT VALUE — one sed command per placeholder. The formats
# reproduce the literal spellings the templates carried before they were
# parameterised, so the generated DTS stays byte-identical while the values are
# unchanged (8-digit hex for addresses, 2-digit hex for the INTx SPI inside
# interrupt-map, decimal for the channel SPI).
emit() {
    local placeholder="$1" format="$2" value="$3" rendered
    rendered="$(printf "${format}" "${value}")"
    printf 's/@%s@/%s/g\n' "${placeholder}" "${rendered}"
}

ch_base="$(macro_dec DANGO_CH_GUEST_BASE)"
ch_size="$(macro_dec DANGO_CH_GUEST_SIZE)"
ch_irq="$(macro_dec DANGO_CH_GUEST_IRQ)"
spi_base="$(macro_dec DANGO_GIC_SPI_BASE)"
ecam_base="$(macro_dec DANGO_GPU_ECAM_BASE)"
ecam_size="$(macro_dec DANGO_GPU_ECAM_SIZE)"
mmio_base="$(macro_dec DANGO_GPU_MMIO_BASE)"
mmio_size="$(macro_dec DANGO_GPU_MMIO_SIZE)"
intx_spi="$(macro_dec DANGO_GPU_INTX_SPI)"
bar4_ipa="$(macro_dec DANGO_GPU_BAR4_IPA)"
bar4_size="$(macro_dec DANGO_GPU_BAR4_SIZE)"
stg_base="$(macro_dec DANGO_GPU_STAGING_BASE)"
stg_size="$(macro_dec DANGO_GPU_STAGING_SIZE)"
shm_base="$(macro_dec DANGO_SHM_GUEST_BASE)"
shm_size="$(macro_dec DANGO_SHM_GUEST_SIZE)"

# The channel DTS `interrupts` cell carries the SPI number, not the vGIC hwirq
# (32 + SPI). Derive it from the header rather than restating it.
ch_spi=$(( ch_irq - spi_base ))
if (( ch_spi <= 0 )); then
    echo "error: channel SPI must be positive (irq=${ch_irq} spi_base=${spi_base})" >&2
    exit 1
fi

# BAR4 must live inside the guest MMIO window the DTS exposes.
if (( bar4_ipa < mmio_base || bar4_ipa + bar4_size > mmio_base + mmio_size )); then
    echo "error: DANGO_GPU_BAR4_IPA (+BAR4_SIZE) is outside DANGO_GPU_MMIO_BASE..+SIZE" >&2
    exit 1
fi

# The staging window is a device-side IPA range above RAM; it must not overlap
# the channel window or the Assigned MMIO window.
if (( stg_base < ch_base + ch_size )); then
    echo "error: DANGO_GPU_STAGING_BASE overlaps the channel window" >&2
    exit 1
fi
if (( stg_base < mmio_base + mmio_size && stg_base + stg_size > mmio_base )); then
    echo "error: DANGO_GPU_STAGING window overlaps the MMIO window" >&2
    exit 1
fi

# The dynamic shared-arena descriptor window (Phase L) must not overlap the
# channel window, the staging window or the GPU MMIO aperture; regions live at
# owner-chosen IPAs and are never placed inside this window.
if (( shm_base < ch_base + ch_size )); then
    echo "error: DANGO_SHM_GUEST_BASE overlaps the channel window" >&2
    exit 1
fi
if (( shm_base < stg_base + stg_size )); then
    echo "error: DANGO_SHM_GUEST_BASE overlaps the staging window" >&2
    exit 1
fi
if (( shm_base < mmio_base + mmio_size && shm_base + shm_size > mmio_base )); then
    echo "error: DANGO_SHM_GUEST window overlaps the MMIO window" >&2
    exit 1
fi

# Placeholders are emitted longest-name-first so a plain name (e.g. BASE) can
# never be substituted inside a longer one (e.g. BASE_UA).
emit DANGO_CH_GUEST_BASE_UA  '%08x'   "${ch_base}"
emit DANGO_GPU_ECAM_BASE_UA  '%08x'   "${ecam_base}"
emit DANGO_GPU_STAGING_BASE_UA '%08x' "${stg_base}"
emit DANGO_SHM_GUEST_BASE_UA '%08x' "${shm_base}"
emit DANGO_GPU_INTX_SPI_1    '0x%02x' "$(( intx_spi + 1 ))"
emit DANGO_GPU_INTX_SPI_2    '0x%02x' "$(( intx_spi + 2 ))"
emit DANGO_GPU_INTX_SPI_3    '0x%02x' "$(( intx_spi + 3 ))"
emit DANGO_CH_GUEST_BASE     '0x%08x' "${ch_base}"
emit DANGO_CH_GUEST_SIZE     '0x%x'   "${ch_size}"
emit DANGO_CH_GUEST_SPI      '%d'     "${ch_spi}"
emit DANGO_CH_GUEST_IRQ      '%d'     "${ch_irq}"
emit DANGO_GPU_ECAM_BASE     '0x%08x' "${ecam_base}"
emit DANGO_GPU_ECAM_SIZE     '0x%08x' "${ecam_size}"
emit DANGO_GPU_MMIO_BASE     '0x%08x' "${mmio_base}"
emit DANGO_GPU_MMIO_SIZE     '0x%08x' "${mmio_size}"
emit DANGO_GPU_STAGING_BASE  '0x%08x' "${stg_base}"
emit DANGO_GPU_STAGING_SIZE  '0x%08x' "${stg_size}"
emit DANGO_SHM_GUEST_BASE   '0x%08x' "${shm_base}"
emit DANGO_SHM_GUEST_SIZE   '0x%x'   "${shm_size}"
emit DANGO_GPU_INTX_SPI      '0x%02x' "${intx_spi}"
