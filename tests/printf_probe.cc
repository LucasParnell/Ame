// tests/printf_probe.cc — host test for libk's bound formatter and checked math.
#include "libk/printf.h"
#include "libk/integer.h"
#include "libk/str.h"

static int failures = 0;
static void check(bool c) { if (!c) ++failures; }

int main() {
    char b[32];

    // Basic formatting.
    check(libk::snprintf(b, sizeof(b), "%s=%d/%u/%x", "x", -5, 42u, 255u) == 10);
    check(libk::str::equal(b, "x=-5/42/ff"));

    // Width / zero pad / left align.
    check(libk::snprintf(b, sizeof(b), "%05d", 42) == 5 && libk::str::equal(b, "00042"));
    check(libk::snprintf(b, sizeof(b), "%-4d|", 7) == 5 && libk::str::equal(b, "7   |"));

    // Truncation: returns the would-be length, stores cap-1 and NULs.
    check(libk::snprintf(b, 4, "%s", "abcdef") == 6);
    check(libk::str::equal(b, "abc"));
    check(libk::snprintf(b, 1, "%s", "abcdef") == 6 && b[0] == '\0');

    // cap == 0 writes nothing.
    b[0] = 'Z';
    check(libk::snprintf(b, 0, "%d", 12345) == 5 && b[0] == 'Z');

    // %p and %zu.
    check(libk::snprintf(b, sizeof(b), "%zu", static_cast<std::size_t>(7)) == 1 && libk::str::equal(b, "7"));

    // Overflow-checked arithmetic.
    unsigned long long v = 123;
    check(libk::checked_add(1, 2, &v) && v == 3);
    check(!libk::checked_add(~0ull, 1ull, &v) && v == 3);   // untouched on failure
    check(libk::checked_sub(5, 2, &v) && v == 3);
    check(!libk::checked_sub(1, 2, &v));
    check(libk::checked_mul(4, 5, &v) && v == 20);
    check(!libk::checked_mul(~0ull, 2ull, &v));
    return failures;
}
