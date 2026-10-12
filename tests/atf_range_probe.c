// Compiled by build-switch-atf.sh against the actual patched firmware function
// and its platform macros, extracted from the verified source archive.
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#define U(x) x##U
#define ULL(x) x##ULL
#define ERROR(...) ((void)0)
static const uint64_t tegra_bl31_phys_base = TZDRAM_BASE;
#include "atf-range-function.inc"

_Static_assert(TZDRAM_END == 0x100000000ULL, "secure aperture end must not wrap at 4 GiB");

int main(void) {
    assert(bl31_check_ns_address(0, 1) != 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_BASE, 0) != 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_BASE - 1, 1) != 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_BASE, UINT64_MAX) != 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_BASE, 1) == 0);
    assert(bl31_check_ns_address(TZDRAM_BASE - 1, 1) == 0);
    assert(bl31_check_ns_address(TZDRAM_BASE - 1, 2) != 0);
    assert(bl31_check_ns_address(TZDRAM_BASE, 1) != 0);
    assert(bl31_check_ns_address(TZDRAM_END - 1, 1) != 0);
    assert(bl31_check_ns_address(TZDRAM_END, 1) == 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_END, 1) == 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_END, 2) != 0);
    assert(bl31_check_ns_address(TEGRA_DRAM_END + 1, 1) != 0);
}
