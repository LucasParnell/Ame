#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include "midix/format/bundle.h"
#include "midix/format/json.h"
#include "anko/fdt.h"
static std::vector<unsigned char> read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    auto data = read(argv[2]);
    const std::string mode(argv[1]);
    if (mode == "json") {
        return midix::json::parse(reinterpret_cast<const char*>(data.data()), data.size()) ? 0 : 1;
    }
    if (mode == "fdt") {
        fdt::Callbacks cb{nullptr, [](void*, const char*, u32) { return true; }, [](void*, u32) {},
                          [](void*, const char*, const u8*, u32, u32) {}};
        return fdt::walk(data.data(), cb, data.size()) &&
                       fdt::walk_memreserve(
                           data.data(), nullptr, [](void*, u64, u64) { return true; }, data.size())
                   ? 0
                   : 1;
    }
    if (mode != "bundle" || argc != 4) {
        return 2;
    }
    auto key = read(argv[3]);
    if (key.size() != 32) {
        return 2;
    }
    midix::format::Bundle bundle;
    auto error = midix::format::verify_bundle(
        data.data(), data.size(), reinterpret_cast<const uint8_t (*)[32]>(key.data()), 1, bundle);
    if (error) {
        std::cerr << error << '\n';
    }
    return error ? 1 : 0;
}
