#include "display.h"
#include <cassert>

int main() {
    using namespace anne::display;
    const Registers valid{1, 5u << 3, 0x5d000000, 270, 0, 0, 0, 0, 1, 1079, 2279};
    auto buffer = validate(valid);
    assert(buffer.address == kLogoBase && buffer.stride == 4320);
    assert(buffer.width == 1080 && buffer.height == 2280);
    auto reject = [&](Registers r) { assert(validate(r).address == 0); };
    auto r = valid; r.dma_control |= 1u << 8; reject(r); // IOVA
    r = valid; r.dma_control |= 1u << 1; reject(r); // tiled
    r = valid; r.dma_control |= 1u << 9; reject(r); // transformed
    r = valid; r.channel_control |= 4; reject(r); // compressed
    r = valid; r.ldi_control = 0; reject(r); // disabled scanout
    r = valid; r.address = 0x00480000; reject(r); // kernel RAM
    r = valid; r.address = kLogoBase + kLogoSize - 4; reject(r); // truncated allocation
    r = valid; r.stride = 269; reject(r); // short row
    r = valid; r.y0 = 1; reject(r); // cropped
    r = valid; r.dma_control = 8u << 3; reject(r); // unsupported YUV
    r = valid; r.dma_control = 0; r.stride = 135;
    assert(validate(r).bytes_per_pixel == 2 && validate(r).stride == 2160);
    r = valid; r.width = 1080; reject(r); // unexpected geometry
    r = valid; r.stride = 272; assert(validate(r).stride == 4352); // padding supported
}
