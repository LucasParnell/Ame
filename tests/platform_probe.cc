// Production DT transform against read-only input, without board MMIO.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "anko/hw_info.h"
#include "anko/boot_reservations.h"

HwInfo g_hw{};
extern "C" { u64 g_boot_el = 1; }
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const int fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    struct stat info{};
    assert(fstat(fd, &info) == 0 && info.st_size > 0);
    void* mapping = mmap(nullptr, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(mapping != MAP_FAILED);
    close(fd);
    HwInfo hw{};
    hw.ram_base = 0x40000000;
    hw.ram_size = 0x4000000;
    hw.console_base = 0xdead0000;
    unsigned char before[sizeof(hw)];
    std::memcpy(before, &hw, sizeof(hw));
    platform::BootReservations reservations{};
    // Reuse an apparently published output: every failure must revoke it.
    reservations.source = static_cast<const u8*>(mapping);
    reservations.count = 99;
    const bool ok = platform::probe_dtb(static_cast<const u8*>(mapping), info.st_size, hw,
                                       argc == 3 ? &reservations : nullptr);
    if (!ok) assert(!std::memcmp(before, &hw, sizeof(hw)));
    if (argc == 3) {
        assert(std::strcmp(argv[2], "reservations") == 0);
        assert(!reservations.source || (ok && !reservations.overflow));
        std::printf("%u %u %u %u %u\n", ok, reservations.source == mapping,
                    reservations.count, reservations.memreserve_count, reservations.overflow);
        if (reservations.source) {
            for (u32 i = 0; i < reservations.count; ++i) {
                std::printf("%llx %llx\n", (unsigned long long)reservations.ranges[i].base,
                            (unsigned long long)reservations.ranges[i].size);
            }
        }
        munmap(mapping, info.st_size);
        return 0;
    }
    std::printf("%u %llx %llx %llx %u %llx %llx %u %llx %u %llx\n", ok,
        (unsigned long long)hw.ram_base, (unsigned long long)hw.ram_size,
        (unsigned long long)hw.console_base, hw.virtio_count,
        (unsigned long long)hw.virtio_base, (unsigned long long)hw.gic_dist_base,
        hw.has_anne_usb, (unsigned long long)hw.gic_cpu_base, hw.has_smmu,
        (unsigned long long)hw.smmu_base);
    munmap(mapping, info.st_size);
}
