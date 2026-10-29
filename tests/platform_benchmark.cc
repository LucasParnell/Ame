// Host benchmark of the production transform, with a read-only mapped DTB.
// CPU caches/timing here are the host's; measure phone checkpoints separately.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "anko/hw_info.h"

HwInfo g_hw{};
extern "C" { u64 g_boot_el = 1; }

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const unsigned iterations = std::strtoul(argv[2], nullptr, 10);
    if (!iterations) return 2;
    const int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return 2;
    struct stat info{};
    if (fstat(fd, &info) || info.st_size <= 0) return 2;
    void* mapping = mmap(nullptr, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) return 2;
    const auto* dtb = static_cast<const u8*>(mapping);
    HwInfo initial{}, result{};
    initial.ram_base = 0x40000000;
    initial.ram_size = 0x4000000;
    for (unsigned i = 0; i < 32; ++i) {
        result = initial;
        if (!platform::probe_dtb(dtb, info.st_size, result)) return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) {
        result = initial;
        if (!platform::probe_dtb(dtb, info.st_size, result)) return 1;
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    // Hash every semantic field, excluding unspecified C++ padding. It also
    // catches accidental accumulation of virtio transports between iterations.
    const u64 fields[]{result.ram_base, result.ram_size, u64(result.console_kind),
        result.console_base, u64(result.intc_kind), result.gic_dist_base, result.gic_cpu_base,
        result.timer_irq, result.boot_el, result.has_fwcfg, result.has_virtio_blk,
        result.has_pcie, result.fwcfg_base, result.virtio_base, result.ecam_base,
        result.has_pci_mmio, result.pci_mmio_base, result.pci_mmio_size,
        result.has_t210_dc, result.t210_dc_fb_phys, result.t210_dc_fb_width,
        result.t210_dc_fb_height, result.xhci_noreset, result.virtio_count,
        result.has_anne_usb, result.has_smmu, result.smmu_base};
    u64 hash = 14695981039346656037ull;
    for (u64 value : fields) hash = (hash ^ value) * 1099511628211ull;
    for (u64 value : result.virtio_bases) hash = (hash ^ value) * 1099511628211ull;
    std::printf("{\"iterations\":%u,\"mean_us\":%.3f,\"hardware_hash\":\"%016llx\"}\n",
                iterations, elapsed / iterations, static_cast<unsigned long long>(hash));
    munmap(mapping, info.st_size);
}
