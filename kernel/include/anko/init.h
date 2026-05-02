#pragma once

#include "anko/types.h"
#include "anko/mm.h"
#include "anko/sched.h"

namespace kernel::init {

bool discover_initrd(PhysAddr dtb_phys, PhysAddr* out_base, usize* out_size);
bool init_archive(PhysAddr initrd_base, usize initrd_size);
bool archive_get(const char* name, const void** out_data, usize* out_size);
bool archive_materialize(PhysAddr* out_base, usize* out_size);

VirtAddr load_elf(VmSpace& space, const void* elf_data, usize elf_size);
bool load_service_elf(const char* service_name, VmSpace& space, VirtAddr* out_entry);
Process* spawn_image(const void* elf_data, usize elf_size, Principal principal,
                     u32 supervisor_pid, u32 flags = 0, Thread** out_main_thread = nullptr);
Process* spawn_service(const char* service_name, Principal principal, u32 supervisor_pid,
                       u32 flags = 0, Thread** out_main_thread = nullptr);
void create_init_process();

} // namespace kernel::init
