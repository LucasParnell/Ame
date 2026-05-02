#pragma once

namespace libk {

[[noreturn]] void panic(const char* msg);

#define ASSERT(cond) \
    do { if (!(cond)) [[unlikely]] libk::panic("assert: " #cond); } while(0)

} // namespace libk
