#include "midix/usb/hid_report.h"
#include <cassert>
#include <cstdio>
using namespace midix::usb::hid;
static constexpr mx_u8_t kKeyboard[] = {
    0x05,1, 0x09,6, 0xa1,1, 0x05,7, 0x19,0xe0, 0x29,0xe7,
    0x15,0, 0x25,1, 0x75,1, 0x95,8, 0x81,2,
    0x95,1, 0x75,8, 0x81,1,
    0x95,5, 0x75,1, 0x05,8, 0x19,1, 0x29,5, 0x91,2,
    0x95,1, 0x75,3, 0x91,1,
    0x95,6, 0x75,8, 0x05,7, 0x19,0, 0x29,0x65, 0x15,0, 0x25,0x65, 0x81,0, 0xc0,
};

static void keyboard_and_leds() {
    Layout layout{};
    assert(compile(libk::as_bytes(kKeyboard), &layout));
    assert(report_bytes(layout.reports[0], Kind::Input) == 8);
    KeyboardDecoder decoder;
    assert(decoder.init(&layout));
    mx_u8_t report[8]{2, 0, 4};
    assert(decoder.prepare(libk::as_bytes(report)));
    bool key = false, shift = false;
    while (const auto* event = decoder.event()) {
        assert(!decoder.prepare(libk::as_bytes(report))); // unconsumed result is retained
        assert(event->type == INPUT_KEY_DOWN && event->modifiers == 2);
        key |= event->keycode == 4;
        shift |= event->keycode == 0xe1;
        assert(decoder.advance());
    }
    assert(key && shift);
    assert(decoder.prepare(libk::as_bytes(report)) && !decoder.event());
    report[0] = 0;
    report[2] = 1; // rollover retains ordinary held keys but updates modifiers
    assert(decoder.prepare(libk::as_bytes(report)));
    assert(decoder.event()->keycode == 0xe1 && decoder.event()->type == INPUT_KEY_UP);
    assert(decoder.advance() && !decoder.event());
    assert(decoder.release_all());
    assert(decoder.event()->keycode == 4 && decoder.event()->type == INPUT_KEY_UP);
    assert(decoder.advance() && !decoder.event());
    mx_u8_t leds[2]{0xff, 0xaa};
    mx_u32_t length = 0;
    assert(encode_leds(layout, 0, 5, libk::as_bytes(leds), &length));
    assert(length == 1 && leds[0] == 5 && leds[1] == 0xaa);
    assert(!encode_leds(layout, 0, 5, {}, &length));
    for (mx_u32_t n = 0; n < sizeof(kKeyboard); ++n) {
        // A prefix that ends inside an item or collection cannot bind.
        assert(!compile(libk::as_bytes(kKeyboard, n), &layout));
        assert(!layout.keyboard && !layout.field_count);
    }
}

static void report_ids() {
    const mx_u8_t descriptor[] = {
        0x05,1, 0x09,6, 0xa1,1, 0x05,7, 0x15,0, 0x25,1, 0x75,1, 0x95,8,
        0x85,1, 0x19,4, 0x29,11, 0x81,2,
        0x85,2, 0x19,0xe0, 0x29,0xe7, 0x81,2, 0xc0,
    };
    Layout layout{};
    assert(compile(libk::as_bytes(descriptor), &layout) && layout.numbered);
    KeyboardDecoder decoder;
    assert(decoder.init(&layout));
    mx_u8_t keys[]{1, 1}, modifiers[]{2, 2};
    assert(decoder.prepare(libk::as_bytes(keys)) && decoder.event()->keycode == 4);
    assert(decoder.advance() && !decoder.event());
    assert(decoder.prepare(libk::as_bytes(modifiers)) && decoder.event()->keycode == 0xe1);
    assert(decoder.advance() && !decoder.event());
    keys[1] = 0;
    assert(decoder.prepare(libk::as_bytes(keys)));
    assert(decoder.event()->keycode == 4 && decoder.event()->modifiers == 2);
    assert(decoder.advance() && !decoder.event());
    const mx_u8_t bad[]{3, 1};
    assert(!decoder.prepare(libk::as_bytes(bad)));
    assert(!decoder.prepare(libk::as_bytes(modifiers, 1)));
    assert(decoder.release_all() && decoder.event()->keycode == 0xe1);
    assert(decoder.advance() && !decoder.event());
}

static void bounded_helpers() {
    mx_u8_t bytes[]{0xaa, 0x55};
    assert(libk::write_bits(libk::as_bytes(bytes), 6, 4, 15));
    assert(bytes[0] == 0xea && bytes[1] == 0x57);
    assert(!libk::write_bits(libk::as_bytes(bytes), 15, 2, 1) && bytes[1] == 0x57);
    assert(!libk::write_bits(libk::as_bytes(bytes), 0, 1, 2) && bytes[0] == 0xea);
    assert(!libk::write_bits(libk::as_bytes(bytes), ~mx_u64_t{0}, 2, 1));
    assert(libk::write_bits({}, 0, 0, 0) && !libk::write_bits({}, 0, 0, 1));
    mx_u8_t wide[8]{};
    assert(libk::write_bits(libk::as_bytes(wide), 0, 64, ~mx_u64_t{0}));
    for (const auto byte : wide) assert(byte == 255);
}
int main() {
    keyboard_and_leds();
    report_ids();
    bounded_helpers();
    std::puts("hid report probe ok");
}
