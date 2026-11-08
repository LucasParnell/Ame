#include "ame/ui.h"
#include "ame/ui_raster.h"
#include <cassert>
#include <climits>
#include <cstdio>

using namespace ame::ui;
int main() {
    assert(add(INT_MAX, 1) == INT_MAX && sub(INT_MIN, 1) == INT_MIN);
    assert(mul(INT_MIN, -1) == INT_MAX);
    assert(text(INT_MIN) == Text("-2147483648"));
    assert(concat(Text("count="), text(7)) == Text("count=7"));
    Text full; full.size = 255;
    assert(concat(full, Text("overflow")).size == 255);
    Scene<6> scene;
    assert(!scene.add_node(Kind::Item, 0));
    assert(scene.add_node(Kind::Column, -1));
    scene.set(0, Property::Width, 120); scene.set(0, Property::Height, 100);
    scene.set(0, Property::Padding, 10); scene.set(0, Property::Spacing, 5);
    scene.set(0, Property::Clip, 1);
    assert(scene.add_node(Kind::Row, 0));
    scene.set(1, Property::Spacing, 3);
    assert(scene.add_node(Kind::Button, 1));
    scene.set(2, Property::Width, 30); scene.set(2, Property::Height, 20);
    assert(scene.add_node(Kind::Button, 1));
    scene.set(3, Property::Width, 40); scene.set(3, Property::Height, 25);
    assert(scene.add_node(Kind::Text, 0)); scene.set_text(4, Text("abc"));
    assert(scene.layout(120, 100));
    assert(scene.geometry(1).box.width == 73 && scene.geometry(1).box.height == 25);
    assert(scene.geometry(2).box.x == 10 && scene.geometry(3).box.x == 43);
    assert(scene.geometry(4).box.y == 40 && scene.geometry(4).box.width == 24);
    assert(scene.hit_test(44, 11) == 3 && scene.hit_test(10, 10) == 2);
    scene.pointer_down(11, 11); assert(scene.pointer_up(44, 11) == -1);
    scene.pointer_down(11, 11); assert(scene.pointer_up(12, 12) == 2);
    scene.pointer_down(11, 11); scene.cancel_input(); assert(scene.pointer_up(12, 12) == -1);
    scene.set(2, Property::Enabled, 0);
    assert(scene.hit_test(11, 11) == 2); // last committed layout until next frame
    assert(scene.layout(120, 100));
    assert(scene.hit_test(11, 11) == -1 && scene.focus_next() == 3);
    scene.set(3, Property::X, 100);
    assert(scene.layout(120, 100));
    assert(scene.hit_test(144, 11) == -1 && scene.focus_next() == -1);
    // Failed/cyclic layout never replaces the last coherent input geometry.
    scene.set(1, Property::Width, -1); scene.set(2, Property::Fill, 1);
    assert(!scene.layout(120, 100) && scene.error() == LayoutError::CyclicSizing);
    assert(scene.geometry(3).box.x == 143);
    scene.set(2, Property::Fill, 0); scene.set(3, Property::X, 0);
    assert(scene.layout(120, 100));
    uint32_t pixels[120 * 100 + 2]; pixels[0] = 0x12345678; pixels[120 * 100 + 1] = 0x87654321;
    RasterSink raster(pixels + 1, 120 * 100, 120, 100, 120);
    assert(raster.clear()); assert(scene.draw(raster)); scene.presented();
    const auto layouts = scene.layouts();
    assert(scene.layout(120, 100) && scene.layouts() == layouts);
    assert(!scene.needs_frame() && !scene.draw(raster) && scene.frames() == 1);
    scene.set(0, Property::Color, 0xffabcdef);
    assert(scene.layout(120, 100));
    RasterSink invalid(nullptr, 0, 120, 100, 120);
    assert(!scene.draw(invalid) && scene.needs_frame());
    assert(raster.clear(0xff0000ff));
    assert(raster.rectangle({0, 0, 1, 1}, {0, 0, 120, 100}, 0x80ff0000));
    assert(pixels[1] == 0xff80007f);
    assert(raster.rectangle({INT_MIN, INT_MIN, INT_MAX, INT_MAX}, {0, 0, 120, 100}, 0xffffffff));
    assert(pixels[0] == 0x12345678 && pixels[120 * 100 + 1] == 0x87654321);
    Scene<16> depth;
    for (int i = 0; i < 12; ++i) assert(depth.add_node(Kind::Item, i - 1));
    assert(!depth.add_node(Kind::Item, 11));
    std::puts("UI runtime: layout, input, clipping, overflow, atomic layout and idle frames verified");
}
