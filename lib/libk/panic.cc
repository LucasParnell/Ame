// lib/libk/panic.cc — print message over UART then halt.
//
// Spec ref: §12 Phase 0 — `panic` path halts with message.
// In V0 we are uniprocessor, so a wfe loop on this core is sufficient.

#include "libk/panic.h"
#include "libk/printf.h"

namespace libk {

[[noreturn]] void panic(const char* msg) {
    puts("\n*** ANKO PANIC: ");
    puts(msg ? msg : "(null)");
    puts(" ***\n");
    for (;;) {
        asm volatile("wfe");
    }
}

} // namespace libk
