// tests/vectors_probe.cc — host check for the exception vector name table.
//
// Prints one "index|reason" line per vector slot so tests/test_panic_vectors.py
// can assert every slot has a reason and the known ones read correctly.

#include <cstdio>

#include "anko/exception_vectors.h"

int main() {
    for (u32 i = 0; i < anko::kExceptionVectorCount; ++i) {
        std::printf("%u|%s\n", i, anko::kExceptionVectorReason[i]);
    }
    return 0;
}
