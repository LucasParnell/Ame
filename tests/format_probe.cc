#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include "midix/format/bundle.h"
#include "midix/format/archive.h"
#include "midix/format/json.h"
#include "midix/format/image.h"
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
    if (mode == "image") {
        mx_u64_t relocated = 0;
        using midix::format::rebase_image_address;
        if (!rebase_image_address(0x12b14000, 0x12b00000, 0x11000000, &relocated) || relocated != 0x11014000 ||
            !rebase_image_address(0x12b14000, 0x12b00000, 0x17000000, &relocated) || relocated != 0x17014000 ||
            rebase_image_address(0x11000000, 0x12b00000, 0x11000000, &relocated) ||
            rebase_image_address(~mx_u64_t{0}, 0, 1, &relocated)) return 2;
        midix::format::ImagePlan plan{};
        return midix::format::parse_image(data.data(), data.size(), plan) ? 1 : 0;
    }
    if (mode == "archive") {
        libk::const_bytes entry{};
        return midix::format::archive_find(libk::as_bytes(data.data(), data.size()), "init", &entry) && entry.size ? 0 : 1;
    }
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
    if ((mode != "bundle" && mode != "bundle-progress") || argc != 4) {
        return 2;
    }
    auto key = read(argv[3]);
    if (key.size() != 32) {
        return 2;
    }
    midix::format::Bundle bundle;
    std::size_t polls = 0;
    integrity::Progress progress{};
    if (mode == "bundle-progress") {
        progress = {[](void* state) { ++*static_cast<std::size_t*>(state); }, &polls};
    }
    auto error = midix::format::verify_bundle(
        data.data(), data.size(), reinterpret_cast<const uint8_t (*)[32]>(key.data()), 1, bundle,
        progress);
    if (error) {
        std::cerr << error << '\n';
    }
    if (!error && mode == "bundle-progress" && polls < data.size() / 4096) return 2;
    if (error && bundle.data) return 2; // Hooks never publish a rejected bundle.
    return error ? 1 : 0;
}
