// tests/libk_probe.cc — host test for the safe libk helpers.
//
// libk/bytes.h, libk/str.h and libk/num.h are pure and host-compilable. This
// exercises the bounds behaviour that is the whole point of them: a wrong length
// must give a short count or a false, never an out-of-bounds access (ASan/UBSan
// catch it if it does).
#include "libk/bytes.h"
#include "libk/str.h"
#include "libk/num.h"

static int failures = 0;

static void check(bool cond) {
    if (!cond) ++failures;
}

int main() {
    // --- bytes -------------------------------------------------------------
    unsigned char dst[8] = {0};
    const unsigned char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    check(libk::copy(libk::as_bytes(dst, 4), libk::as_bytes(src, 8)) == 4);
    check(dst[0] == 1 && dst[3] == 4 && dst[4] == 0);
    check(libk::copy_exact(libk::as_bytes(dst, 8), libk::as_bytes(src, 8)));
    for (int i = 0; i < 8; ++i) check(dst[i] == src[i]);
    check(!libk::copy_exact(libk::as_bytes(dst, 4), libk::as_bytes(src, 8)));

    libk::fill(libk::as_bytes(dst, 8), 0xAB);
    for (int i = 0; i < 8; ++i) check(dst[i] == 0xAB);

    check(libk::copy_n(libk::as_bytes(dst, 8), libk::as_bytes(src, 8), 8));
    check(!libk::copy_n(libk::as_bytes(dst, 2), libk::as_bytes(src, 8), 8)); // dst too small
    check(!libk::copy_n(libk::as_bytes(dst, 8), libk::as_bytes(src, 2), 8)); // src too small
    check(!libk::fill_n(libk::as_bytes(dst, 2), 0, 8));                      // dst too small

    // A null pointer normalises to an empty view and is never dereferenced.
    check(libk::as_bytes(static_cast<void*>(nullptr), 16).size == 0);
    check(libk::copy(libk::as_bytes(static_cast<void*>(nullptr), 16),
                     libk::as_bytes(static_cast<const void*>(nullptr), 16)) == 0);

    // slice bounds.
    const libk::const_bytes view = libk::as_bytes(src, 8);
    check(libk::slice(view, 2, 3).size == 3);
    check(libk::slice(view, 2, 3).data[0] == 3);
    check(libk::slice(view, 6, 3).size == 0);
    check(libk::slice(view, 9, 1).size == 0);

    // --- str ---------------------------------------------------------------
    check(libk::str::length("hello") == 5);
    check(libk::str::length(static_cast<const char*>(nullptr)) == 0);
    check(libk::str::length_n("hello", 3) == 3);
    check(libk::str::equal("abc", "abc"));
    check(!libk::str::equal("abc", "abd"));
    check(!libk::str::equal("abc", "abcd"));
    check(libk::str::equal_n("abcx", "abcy", 3));
    check(!libk::str::equal_n("abcx", "abcy", 4));
    check(libk::str::starts_with("hello", "he"));
    check(!libk::str::starts_with("he", "hello"));

    char sbuf[5];
    check(libk::str::copy_into(sbuf, 4, "abcd") == 3); // truncated but terminated
    check(sbuf[3] == '\0');
    check(!libk::str::copy_exact_fit(sbuf, 4, "abcd"));
    check(libk::str::copy_exact_fit(sbuf, 5, "abcd"));
    check(libk::str::equal(sbuf, "abcd"));
    check(libk::str::copy_into(sbuf, 0, "x") == 0); // cap 0 writes nothing

    // --- num ---------------------------------------------------------------
    unsigned long long v = 0;
    check(libk::num::parse_u64("18446744073709551615", &v) && v == 0xFFFFFFFFFFFFFFFFull);
    check(!libk::num::parse_u64("18446744073709551616", &v)); // overflow rejected
    check(!libk::num::parse_u64("12x", &v));
    check(!libk::num::parse_u64("", &v));
    check(!libk::num::parse_u64("0x", &v));
    check(libk::num::parse_u64("0x1f", &v) && v == 31);

    char nbuf[32];
    check(libk::num::to_dec(12345, nbuf, sizeof(nbuf)) == 5 && libk::str::equal(nbuf, "12345"));
    check(libk::num::to_hex(0, nbuf, sizeof(nbuf)) == 3 && libk::str::equal(nbuf, "0x0"));
    check(libk::num::to_hex(255, nbuf, sizeof(nbuf)) == 4 && libk::str::equal(nbuf, "0xff"));
    check(libk::num::to_dec(7, nbuf, 2) == 1 && nbuf[1] == '\0'); // tiny cap stays terminated

    return failures;
}
