#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "midix/format/app_package.h"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
    midix::package::View package{};
    if (!midix::package::parse(bytes.data(), bytes.size(), package)) {
        if (package.data || package.count) return 3;
        return 1;
    }
    if (argc > 2) {
        const auto resource = package.find(argv[2]);
        if (!resource) return 4;
        if (resource.size) std::fwrite(resource.data, 1, resource.size, stdout);
    }
    return 0;
}
