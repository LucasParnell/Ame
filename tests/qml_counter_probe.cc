#include "Counter.h"
#include "ame/ui_raster.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static uint32_t pixels[360 * 240];
static void save(const char* path) {
    auto* file = std::fopen(path, "wb"); assert(file);
    std::fprintf(file, "P6\n360 240\n255\n");
    for (auto color : pixels) {
        const unsigned char rgb[] = {static_cast<unsigned char>(color >> 16),
                                    static_cast<unsigned char>(color >> 8),
                                    static_cast<unsigned char>(color)};
        std::fwrite(rgb, 1, 3, file);
    }
    std::fclose(file);
}
int main(int argc, char** argv) {
    ame::qml::Counter app;
    ame::ui::RasterSink raster(pixels, 360 * 240, 360, 240, 360);
    assert(app.get_count() == 0 && app.get_doubled() == 0);
    assert(app.prepare(360, 240) && raster.clear() && app.scene.draw(raster));
    if (argc > 1) save(argv[1]);
    app.scene.presented();
    assert(!app.scene.needs_frame());
    app.pointer_down(30, 80); assert(app.pointer_up(31, 81));
    assert(app.prepare(360, 240) && app.get_count() == 1 && app.get_doubled() == 2);
    assert(app.scene.node(5).label == ame::ui::Text("Count: 1 / doubled: 2"));
    assert(raster.clear() && app.scene.draw(raster)); app.scene.presented();
    if (argc > 2) save(argv[2]);
    const auto layouts = app.scene.layouts();
    for (unsigned i = 0; i < 100; ++i) assert(app.prepare(360, 240) && !app.scene.needs_frame());
    assert(app.scene.layouts() == layouts && app.scene.frames() == 2);
    app.set_count(1); assert(app.prepare(360, 240) && !app.scene.needs_frame());
    app.set_active(false); assert(app.prepare(360, 240));
    app.pointer_down(30, 80); assert(!app.pointer_up(30, 80));
    assert(app.get_count() == 1);
    app.scene.focus_next(); assert(app.activate_focus());
    assert(app.prepare(360, 240) && app.get_count() == 0);
    std::puts("QML counter: generated bindings, state, pointer/focus activation and idle behavior verified");
}
