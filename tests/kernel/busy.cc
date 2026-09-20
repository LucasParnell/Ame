#include "mochi/syscall.h"
extern "C" [[noreturn]] void _start() {
    while (true) {
        sys_yield();
    }
}
