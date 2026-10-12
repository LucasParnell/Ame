#include <cstdio>
#include <cstdlib>
#include "anko/cpu_topology.h"
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    FILE* f = std::fopen(argv[1], "rb"); if (!f) return 3;
    std::fseek(f, 0, SEEK_END); const auto size = std::ftell(f); std::rewind(f);
    auto* data = static_cast<unsigned char*>(std::malloc(size));
    if (!data || std::fread(data, 1, size, f) != static_cast<unsigned long>(size)) return 4;
    std::fclose(f);
    const auto t = smp::read_cpu_topology(reinterpret_cast<PhysAddr>(data), std::strtoull(argv[2], nullptr, 0));
    std::free(data);
    std::printf("%u %u %u %llx", t.count, t.psci_available, t.psci_hvc, (unsigned long long)t.cpu_on);
    for (u32 i = 0; i < t.count; ++i) std::printf(" %llx", (unsigned long long)t.mpidr[i]);
    std::puts("");
}
