// kernel/init/elf_loader.cc — minimal ELF64 static loader.
//
// Spec ref: §5.1 Boot (step 13: "load init from the boot image")
//
// Loads a statically-linked ELF64 LE AArch64 binary from the initrd
// into a new user address space. No dynamic linking, no relocation.
//
// Steps:
//   1. Validate ELF magic, class=64, data=LE, type=ET_EXEC, machine=AArch64.
//   2. Walk PT_LOAD segments; for each:
//      a. Allocate physical pages (ceil(filesz / PAGE_SIZE)).
//      b. Copy segment data from initrd.
//      c. Map pages into target VmSpace with correct rights (R/RX/RW).
//   3. Return entry point virtual address.
//
// Does not support:
//   - PIE / ET_DYN (V0 binaries are ET_EXEC at fixed VA).
//   - TLS segments.
//   - GNU stack notes.

#include "anko/init.h"
#include "libk/string.h"

namespace kernel::init {

namespace {

constexpr u32 PT_LOAD = 1;
constexpr u32 PF_X = 1;
constexpr u32 PF_W = 2;
constexpr u32 PF_R = 4;
constexpr u16 ET_EXEC = 2;
constexpr u16 EM_AARCH64 = 183;

struct Elf64Ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct Elf64Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

bool check_ident(const u8* id) {
    return id[0] == 0x7F && id[1] == 'E' && id[2] == 'L' && id[3] == 'F'
        && id[4] == 2 && id[5] == 1;
}

} // namespace

VirtAddr load_elf(VmSpace& space, const void* elf_data, usize elf_size) {
    if (!elf_data || elf_size < sizeof(Elf64Ehdr)) return 0;

    const auto* eh = static_cast<const Elf64Ehdr*>(elf_data);
    if (!check_ident(eh->e_ident)) return 0;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_AARCH64) return 0;
    if (eh->e_phentsize != sizeof(Elf64Phdr)) return 0;
    if (eh->e_phoff + static_cast<u64>(eh->e_phnum) * sizeof(Elf64Phdr) > elf_size) return 0;

    const auto* ph = reinterpret_cast<const Elf64Phdr*>(
        static_cast<const u8*>(elf_data) + eh->e_phoff);

    for (u16 i = 0; i < eh->e_phnum; ++i) {
        const Elf64Phdr& seg = ph[i];
        if (seg.p_type != PT_LOAD) continue;
        if (seg.p_offset + seg.p_filesz > elf_size) return 0;
        if (seg.p_memsz < seg.p_filesz) return 0;
        if (seg.p_vaddr == 0) return 0;
        if ((seg.p_flags & PF_W) && (seg.p_flags & PF_X)) return 0;

        const VirtAddr va_start = static_cast<VirtAddr>(seg.p_vaddr & ~(static_cast<u64>(PAGE_SIZE - 1)));
        const u64 page_off = seg.p_vaddr - va_start;
        u64 map_size = 0;
        if (__builtin_add_overflow(page_off, seg.p_memsz, &map_size)) return 0;
        map_size = (map_size + PAGE_SIZE - 1) & ~(static_cast<u64>(PAGE_SIZE - 1));

        u32 rights = 0;
        if (seg.p_flags & PF_R) rights |= RIGHT_READ;
        if (seg.p_flags & PF_W) rights |= RIGHT_WRITE;
        if (seg.p_flags & PF_X) rights |= RIGHT_EXEC;
        if (rights == 0) rights = RIGHT_READ;

        for (u64 off = 0; off < map_size; off += PAGE_SIZE) {
            PhysAddr page = mm::alloc_page();
            if (!page) return 0;
            libk::memset(reinterpret_cast<void*>(page), 0, PAGE_SIZE);

            const u64 seg_file_start = seg.p_offset;
            const u64 page_seg_start = off;
            const u64 page_seg_end = off + PAGE_SIZE;
            const u64 file_start = page_seg_start > page_off ? page_seg_start - page_off : 0;
            if (file_start < seg.p_filesz) {
                u64 file_end = page_seg_end > page_off ? page_seg_end - page_off : 0;
                if (file_end > seg.p_filesz) file_end = seg.p_filesz;
                if (file_end > file_start) {
                    const u64 dst_off = (page_off + file_start) - page_seg_start;
                    const auto* src = static_cast<const u8*>(elf_data) + seg_file_start + file_start;
                    libk::memcpy(reinterpret_cast<u8*>(page) + dst_off,
                                 src,
                                 static_cast<usize>(file_end - file_start));
                }
            }

            if (!mm::vm_map(space, va_start + off, page, rights)) return 0;
        }
    }

    return static_cast<VirtAddr>(eh->e_entry);
}

bool load_service_elf(const char* service_name, VmSpace& space, VirtAddr* out_entry) {
    if (!service_name || !out_entry) return false;
    const void* blob = nullptr;
    usize blob_size = 0;
    if (!archive_get(service_name, &blob, &blob_size)) return false;
    const VirtAddr entry = load_elf(space, blob, blob_size);
    if (!entry) return false;
    *out_entry = entry;
    return true;
}

} // namespace kernel::init
