// tests/libk_probe.cc — host test for the safe libk helpers.
//
// libk/bytes.h, libk/str.h and libk/num.h are pure and host-compilable. This
// exercises the bounds behaviour that is the whole point of them: a wrong length
// must give a short count or a false, never an out-of-bounds access (ASan/UBSan
// catch it if it does).
#include "libk/bytes.h"
#include "libk/str.h"
#include "libk/num.h"
#include "libk/integer.h"
#include "libk/text.h"
#include "libk/endian.h"
#include "libk/hash.h"

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
    check(libk::slice(libk::bytes{}, 0, 0).data == nullptr);
    check(libk::slice(libk::const_bytes{}, 0, 0).data == nullptr);
    check(libk::as_bytes(src).size == sizeof(src));
    check(libk::as_bytes(dst).size == sizeof(dst));
    check(!libk::copy_n({nullptr, 8}, view, 1));
    check(!libk::copy_exact(libk::as_bytes(dst), {nullptr, 8}));
    check(libk::copy(libk::as_bytes(dst), {nullptr, 8}) == 0);
    check(!libk::fill_n({nullptr, 8}, 0, 1));
    libk::fill({nullptr, 8}, 0); // invalid direct view must not dereference null
    check(libk::copy_n({}, {}, 0));
    check(libk::zero(dst));
    for (auto byte : dst) check(byte == 0);
    struct Record { unsigned char tag; std::uint32_t count; } record;
    libk::fill(libk::as_bytes(&record, sizeof(record)), 0xAB);
    check(libk::zero(record));
    const auto* representation = reinterpret_cast<const unsigned char*>(&record);
    for (std::size_t i = 0; i < sizeof(record); ++i) check(representation[i] == 0);
    libk::fill(libk::as_bytes(dst), 0xAB);
    check(libk::zero(libk::slice(libk::as_bytes(dst), 2, 3)));
    check(dst[1] == 0xAB && dst[2] == 0 && dst[4] == 0 && dst[5] == 0xAB);
    check(libk::zero(dst + 5, 2) && dst[5] == 0 && dst[6] == 0 && dst[7] == 0xAB);
    check(!libk::zero(nullptr, 1));
    check(libk::zero(nullptr, 0));

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
    const char unterminated[4] = {'a', 'b', 'c', 'd'};
    check(!libk::str::copy_exact_fit(sbuf, 4, unterminated));
    check(libk::str::equal(sbuf, "abcd")); // failed copy leaves destination intact

    // Ceiling division must not wrap the numerator at UINT64_MAX.
    constexpr auto max = ~std::uint64_t{0};
    static_assert(libk::div_round_up(max, 1) == max);
    static_assert(libk::div_round_up(max, 2) == (max / 2) + 1);
    static_assert(libk::div_round_up(max, max) == 1);
    static_assert(libk::div_round_up(0, 3) == 0);

    // --- num ---------------------------------------------------------------
    std::uint64_t v = 0;
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

    // Overlap in both directions, aligned bulk moves, and every byte tail.
    alignas(8) unsigned char move[64];
    for (unsigned count = 0; count <= 32; ++count) {
        for (unsigned shift = 1; shift <= 16; ++shift) {
            for (unsigned i = 0; i < sizeof(move); ++i) move[i] = i;
            check(libk::move_n(libk::as_bytes(move + shift, sizeof(move) - shift),
                               libk::as_bytes(move), count));
            for (unsigned i = 0; i < count; ++i) check(move[shift + i] == i);
            for (unsigned i = 0; i < sizeof(move); ++i) move[i] = i;
            check(libk::move_n(libk::as_bytes(move),
                               libk::as_bytes(move + shift, sizeof(move) - shift), count));
            for (unsigned i = 0; i < count; ++i) check(move[i] == shift + i);
        }
    }
    check(!libk::move_n(libk::as_bytes(move, 1), libk::as_bytes(move), 2));
    check(!libk::move_n({nullptr, 1}, libk::as_bytes(move), 1));
    check(!libk::is_zero({nullptr, 1}));
    libk::zero(move);
    check(libk::is_zero(libk::as_bytes(move)));

    char text_storage[8];
    libk::str::Buffer text(text_storage);
    check(text.append("abc") && text.append('d'));
    check(text.format("%u", 12u) && text.length == 6);
    check(!text.append("XYZ") && text.truncated && text.length == 7);
    check(libk::str::equal(text.data, "abcd12X"));
    libk::str::Buffer empty(nullptr, 4);
    check(!empty.append('x') && empty.truncated);
    char one[1];
    libk::str::Buffer terminator(one);
    check(terminator.append("") && one[0] == 0 && !terminator.append('a'));

    std::uint64_t number = 7;
    check(libk::num::parse_u64_digits("777", 3, 8, &number) && number == 511);
    check(!libk::num::parse_u64_digits("8", 1, 8, &number) && number == 511);
    check(libk::num::parse_u64("0x12Z", 4, &number) && number == 18);
    check(!libk::num::parse_u64("0x", 2, &number) && number == 18);
    check(!libk::checked_align_up(~std::uint64_t{0}, 16, &number) && number == 18);
    check(!libk::checked_align_up(5, 3, &number));
    check(libk::checked_align_up(5, 8, &number) && number == 8);
    check(libk::magnitude(-9223372036854775807ll - 1) == (std::uint64_t{1} << 63));
    const unsigned char endian[] = {0xff, 0x12, 0x34, 0x56, 0x78};
    std::uint32_t field = 99;
    check(libk::read_le<std::uint32_t>(libk::slice(libk::as_bytes(endian), 1, 4), &field));
    check(field == 0x78563412);
    check(!libk::read_le<std::uint32_t>(libk::as_bytes(endian, 3), &field) && field == 0x78563412);
    const char crc_vector[] = "123456789";
    check(libk::crc32_ieee(libk::as_bytes(crc_vector, 9)) == 0xcbf43926);
    auto crc = libk::crc32_ieee_update(libk::kCrc32IeeeInit, libk::as_bytes(crc_vector, 4));
    crc = libk::crc32_ieee_update(crc, libk::as_bytes(crc_vector + 4, 5));
    check(~crc == 0xcbf43926);
    check(libk::fnv1a32(libk::as_bytes("hello", 5)) == 0x4f9f2cab);

    return failures;
}
