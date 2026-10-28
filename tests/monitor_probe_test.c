#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void* map_at(uintptr_t address, size_t size) {
    void* p = mmap((void*)address, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(p == (void*)address);
    return p;
}
static size_t load(const char* path, void* destination) {
    FILE* f = fopen(path, "rb");
    assert(f);
    size_t n = fread(destination, 1, 4096, f);
    assert(n && !ferror(f) && feof(f));
    fclose(f);
    return n;
}
int main(int argc, char** argv) {
    assert(argc == 3);
    uint8_t* code = map_at(0x20000000, 8192);
    load(argv[1], code);
    load(argv[2], code + 4096);
    __builtin___clear_cache((char*)code, (char*)code + 8192);
    assert(mprotect(code, 8192, PROT_READ | PROT_EXEC) == 0);
    uint32_t* dmss = map_at(0xfffc0000, 0x8000);
    uint32_t* pctrl = map_at(0xe8a09000, 4096);
    for (unsigned i = 0; i < 0x8000 / 4; ++i) {
        dmss[i] = 0x80000000u | i;
    }
    pctrl[0xc0c / 4] = 0x12345678;
    pctrl[0x68 / 4] = 0x89abcdef;
    int (*probe)(uint64_t, uint32_t) = (void*)code;
    const uint32_t magic = 0x414d4552;
    assert((uint32_t)probe(1, magic) == 0x414d0003);
    assert((uint32_t)probe(2, magic) == dmss[0x6000 / 4]);
    assert((uint32_t)probe(3, magic) == dmss[0x6004 / 4]);
    assert((uint32_t)probe(4, magic) == pctrl[0xc0c / 4]);
    assert((uint32_t)probe(5, magic) == pctrl[0x68 / 4]);
    const unsigned offsets[] = {0x00c, 0x200, 0x208, 0x210, 0x214, 0x218, 0x21c, 0x424};
    for (unsigned i = 0; i < 56; ++i) {
        assert((uint32_t)probe(16 + i, magic) == dmss[((i / 8) * 0x800 + offsets[i % 8]) / 4]);
    }
    for (unsigned i = 0; i < 64; ++i) {
        assert((uint32_t)probe(0x80 + i, magic) == dmss[0x3500 / 4 + i]);
    }
    const uint64_t invalid[] = {0, 7, 15, 72, UINT64_MAX, 0x100000001ull, 0x100000010ull};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(probe(invalid[i], magic) == -1);
    }
    assert(probe(1, magic ^ 1) == -1);
    const uint32_t original_statistics = dmss[0x6260 / 4];
    assert(probe(0x17f, magic ^ 1) == -1);
    assert(dmss[0x6260 / 4] == original_statistics);
    assert((uint32_t)probe(6, magic) == original_statistics);
    assert((uint32_t)probe(0x17f, magic) == original_statistics);
    assert(dmss[0x6260 / 4] == (original_statistics | 0x7f));
    assert((uint32_t)probe(0x100 | (original_statistics & 0x7f), magic) ==
           (original_statistics | 0x7f));
    assert(dmss[0x6260 / 4] == original_statistics);
    assert(probe(0x180, magic) == -1 && dmss[0x6260 / 4] == original_statistics);
    assert(probe(0xc0, magic) == -1 && probe(0xff, magic) == -1);
    assert(probe(0x10000017full, magic) == -1);

    const size_t bytes = 113408;
    uint8_t* staging = map_at(0x03000000, 0x1c000);
    uint8_t* destination = map_at(0x35e00000, 0x1c000);
    memset(staging, 0x5a, bytes);
    // Mock BL31 entry records handoff x1/x2 through preserved x0, then returns.
    const uint32_t entry[] = {0xa9000801, 0x52824680, 0xd65f03c0};
    memcpy(staging, entry, sizeof(entry));
    uint64_t captured[2] = {0};
    uint32_t (*preload)(uint64_t*, uint64_t, uintptr_t) = (void*)(code + 4096);
    const uint64_t handoff_magic = 0x0f1e2d3c4b5a6978ull;
    assert(preload(captured, handoff_magic, (uintptr_t)destination) == 0x1234);
    assert(captured[0] == handoff_magic && captured[1] == (uintptr_t)destination);
    assert(memcmp(staging, destination, bytes) == 0);
    puts("monitor selector bounds and preload handoff passed");
}
