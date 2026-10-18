#pragma once

#include "anko/sched.h"

namespace ipc {

// Syscall execution already retains its process. Reuse that identity instead
// of looking it up under the global process/registry locks for every message.
// Boot setup and sharing to a different process retain the checked lookup.
inline object::Ref<Process> process_reference(u32 pid) {
    Process* current = sched::current_process();
    if (current && current->pid == pid)
        return object::Ref<Process>(current, current->lifetime);
    return sched::find_process(pid);
}

inline cap::Ref resolve_entry(u32 pid, CapHandle handle, CapType type, u32 rights) {
    const auto lookup = [&](Process* p) -> cap::Ref {
        if (!p || !p->cap_table) return nullptr;
        auto entry = cap::lookup(*p->cap_table, handle, type);
        if (!entry || (entry->rights_mask & rights) != rights) return nullptr;
        return entry;
    };
    Process* current = sched::current_process();
    // The executing thread's owner reference lasts through syscall return.
    // Only the capability borrow needs an extra retain in this hot branch.
    if (current && current->pid == pid) return lookup(current);
    auto process = sched::find_process(pid);
    return lookup(process.get());
}

} // namespace ipc
