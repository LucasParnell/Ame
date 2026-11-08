#include "mochi/app.h"
#include "Counter.h"
#include "ame/ui_raster.h"

// Freestanding execution of the compiled QML component. This verifies the
// runtime, package resources and CPU reference pixels; display/input service
// integration follows.
static uint32_t pixels[360 * 240];
static bool good = true;
static void stage(const char* name) {
    if (good) return;
    mochi::app::puts("qml app: failed stage ");
    mochi::app::puts(name);
    mochi::app::puts("\n");
    mochi::app::exit(1);
}
static uint32_t checksum() {
    uint32_t value = 2166136261u;
    for (const auto pixel : pixels) value = (value ^ pixel) * 16777619u;
    return value;
}
// Vulkan requires 4-byte aligned little-endian modules.
static bool module(const midix::package::Bytes& bytes) {
    return bytes && bytes.size >= 20 && !(bytes.size % 4) &&
           bytes.data[0] == 0x03 && bytes.data[1] == 0x02 &&
           bytes.data[2] == 0x23 && bytes.data[3] == 0x07;
}
extern "C" [[noreturn]] void _start() {
    auto& context = mochi::app::start();
    // Freestanding startup has no global constructor runner. Construct the
    // component here, using static storage to preserve the small app stack.
    static ame::qml::Counter counter;
    good = context.resource("ui/Counter.json").size > 0;
    stage("metadata");
    // Device-agnostic modules the renderer compiles once through the driver.
    good = module(context.resource("ui/solid.vert.spv")) &&
           module(context.resource("ui/solid.frag.spv")) &&
           module(context.resource("ui/glyph.vert.spv")) &&
           module(context.resource("ui/glyph.frag.spv"));
    stage("shader resources");
    ame::ui::RasterSink raster(pixels, 360 * 240, 360, 240, 360);
    good = counter.prepare(360, 240) && raster.clear() && counter.scene.draw(raster);
    stage("initial frame");
    const auto before = checksum();
    counter.scene.presented();
    counter.pointer_down(30, 80);
    good = counter.pointer_up(30, 80) && counter.prepare(360, 240);
    stage("pointer activation");
    good = counter.get_count() == 1 && counter.get_doubled() == 2;
    stage("state update");
    good = counter.scene.node(5).label == ame::ui::Text("Count: 1 / doubled: 2");
    stage("bound label");
    good = raster.clear() && counter.scene.draw(raster);
    stage("second frame");
    good = checksum() != before;
    stage("visual change");
    counter.scene.presented();
    const auto layouts = counter.scene.layouts();
    for (unsigned i = 0; i < 100; ++i)
        good = good && counter.prepare(360, 240) && !counter.scene.needs_frame();
    good = good && counter.scene.layouts() == layouts && counter.scene.frames() == 2;
    stage("idle");
    counter.set_count(1);
    good = counter.prepare(360, 240) && !counter.scene.needs_frame();
    stage("unchanged write");
    counter.set_active(false);
    good = counter.prepare(360, 240);
    counter.pointer_down(30, 80);
    good = good && !counter.pointer_up(30, 80) && counter.get_count() == 1;
    stage("disabled button");
    counter.scene.focus_next();
    good = counter.activate_focus() && counter.prepare(360, 240) && counter.get_count() == 0;
    stage("focus activation");
    mochi::app::puts("qml app: bindings, layout, input, resources, raster and idle verified\n");
    mochi::app::exit(0);
}
