#include "midix/abi/protocols/framebuffer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include <cassert>
extern "C" void old_clear(uint32_t*, uint32_t);
extern "C" void new_clear(uint32_t*, uint32_t);
extern "C" void old_text(uint32_t*, const FbDrawTextCmd&);
extern "C" void new_text(uint32_t*, const FbDrawTextCmd&);
constexpr unsigned width = 1080, height = 2280;
std::vector<uint32_t> a(width* height), b(width* height);
template <class Fn> double time(Fn fn, unsigned count) {
    double results[7];
    for (auto& result : results) {
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < count; ++i) {
            fn(i);
        }
        result = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin)
                     .count() /
                 count;
    }
    std::sort(results, results + 7);
    return results[3];
}
int main() {
    auto before = time([](unsigned i) { old_clear(a.data(), i); }, 300);
    auto after = time([](unsigned i) { new_clear(b.data(), i); }, 300);
    assert(a == b);
    std::printf("clear 1080x2280: old %.1f us, new %.1f us, %.2fx\n", before / 1000, after / 1000,
                before / after);
    for (unsigned scale : {1u, 2u, 3u, 8u}) {
        FbDrawTextCmd c{};
        c.x = 64;
        c.y = 224;
        c.len = 28;
        c.scale_x = c.scale_y = scale;
        c.fg = 0xe6f1ff;
        c.bg = 0x001020;
        for (unsigned i = 0; i < 28; ++i) {
            c.text[i] = 'A' + i % 26;
        }
        before = time(
            [&](unsigned i) {
                c.y = 224 + (i % 32) * 16;
                old_text(a.data(), c);
            },
            15000);
        after = time(
            [&](unsigned i) {
                c.y = 224 + (i % 32) * 16;
                new_text(b.data(), c);
            },
            15000);
        assert(a == b);
        std::printf("text 28 chars scale %u: old %.1f ns, new %.1f ns, %.2fx\n", scale, before,
                    after, before / after);
    }
}
