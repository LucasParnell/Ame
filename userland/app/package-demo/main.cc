#include "mochi/app.h"
#include "midix/format/image.h"

// A package/resource smoke app. The UI compiler will supply compiled screens
// through the same executable and resource contract.
extern "C" [[noreturn]] void _start() {
    auto& app = mochi::app::start();
    const auto image = app.self_image();
    static midix::format::ImagePlan plan;
    const auto label = app.resource("ui/label.txt");
    constexpr char expected[] = "Ame packaged UI resources\n";
    bool valid = image && !midix::format::parse_image(image.data, image.size, plan) &&
                 label && label.size == sizeof(expected) - 1 &&
                 !app.resource("ui/missing");
    for (mx_u32_t i = 0; valid && i < label.size; ++i)
        valid = label.data[i] == static_cast<mx_u8_t>(expected[i]);
    // The QEMU fixture adds a large resource to verify bytes across snapshot
    // arena boundaries after the parent has wiped its upload buffer.
    const auto padding = app.resource("ui/padding");
    for (mx_u32_t i = 0; valid && padding && i < padding.size; ++i)
        valid = padding.data[i] == static_cast<mx_u8_t>(i);
    // Repeated lookup must preserve the same immutable mapping.
    valid = valid && app.resource("ui/label.txt").data == label.data &&
            app.self_image().data == image.data;
    if (valid) mochi::app::puts("packaged app: resources verified\n");
    mochi::app::exit(valid ? 0 : 1);
}
