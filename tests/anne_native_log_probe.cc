#include "native_log.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* input = fopen(argv[1], "rb");
    if (!input) return 2;
    fseek(input, 0, SEEK_END);
    long length = ftell(input);
    if (length < 40 || length > 1024 * 1024) return 2;
    rewind(input);
    void* data = malloc(static_cast<size_t>(length));
    if (!data || fread(data, 1, static_cast<size_t>(length), input) != static_cast<size_t>(length)) return 2;
    fclose(input);
    // Never initialize the physical journal in host tests; inspect only its DTB.
    PhysAddr address = 0;
    const bool valid = anne::native_log_layout(reinterpret_cast<PhysAddr>(data), &address);
    printf("%d %llx\n", valid, static_cast<unsigned long long>(address));
    free(data);
}
