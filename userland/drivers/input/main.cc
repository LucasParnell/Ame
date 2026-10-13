// userland/drivers/input/main.cc — ui.input normalization service (M6).
//
// Accepts raw key InputEvents from drv.usb.hid, normalizes US boot-keyboard
// layout to text events, and forwards to a subscriber ring (V0: tty.fb0).
//
// Protocol: INPUT_MSG_SUBSCRIBE to register, INPUT_MSG_EVENT pushed to subscriber.
// Cap policy: drv.usb.hid read ring + tty.fb0 write ring.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/input.h"
#include "mochi/va_layout.h"
#include "mochi/state.h"
#include "mochi/driver.h"

using namespace mochi::va::input;

constexpr uint32_t kSources = 16;
struct Source {
    uint64_t token, generation, sequence;
    uint32_t modifiers, repeat_key;
    uint64_t repeat_at;
    uint8_t held[32];
};
struct Runtime {
    Source sources[kSources];
    mochi::input::EventSource hid, test;
    mochi::SpscEndpoint readers[2];
    uint32_t next_reader;
    mochi::input::EventSink tty;
    mochi::input::LedSink leds;
    InputEvent pending[3]; // optional source reset, raw event, optional text
    uint32_t pending_count, pending_next;
    uint8_t locks;
    bool leds_dirty;
};
static Runtime* g;

// USB HID boot keyboard usage ID -> US ASCII, unshifted and shifted.
static const char kHidToAscii[128] = {
    0,   0,   0,   0,   'a',  'b', 'c', 'd',  'e', 'f', 'g',  'h', 'i',  'j',  'k',
    'l', 'm', 'n', 'o', 'p',  'q', 'r', 's',  't', 'u', 'v',  'w', 'x',  'y',  'z',
    '1', '2', '3', '4', '5',  '6', '7', '8',  '9', '0', '\n', 0,   '\b', '\t', ' ',
    '-', '=', '[', ']', '\\', 0,   ';', '\'', '`', ',', '.',  '/',
};

static const char kHidToAsciiShifted[128] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G',  'H', 'I',  'J',  'K',
    'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',  'W', 'X',  'Y',  'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 0,   '\b', '\t', ' ',
    '_', '+', '{', '}', '|', 0,   ':', '"', '~', '<', '>',  '?',
};

static char ascii_for_key(uint32_t keycode, uint32_t modifiers) {
    if (keycode >= 128) {
        return 0;
    }
    bool shifted = (modifiers & (INPUT_MOD_LSHIFT | INPUT_MOD_RSHIFT)) != 0;
    if ((g->locks & 2u) && keycode >= 4 && keycode <= 29) {
        shifted = !shifted;
    }
    return shifted ? kHidToAsciiShifted[keycode] : kHidToAscii[keycode];
}

static char control_for_key(uint32_t keycode, uint32_t modifiers) {
    const bool ctrl = (modifiers & (INPUT_MOD_LCTRL | INPUT_MOD_RCTRL)) != 0;
    if (!ctrl) {
        return 0;
    }
    if (keycode >= 4 && keycode <= 29) {
        return static_cast<char>(1 + (keycode - 4)); // Ctrl-A..Ctrl-Z
    }
    return 0;
}

static bool flush_pending() {
    while (g->pending_next < g->pending_count) {
        if (!g->tty.try_send(g->pending[g->pending_next])) {
            return false;
        }
        ++g->pending_next;
    }
    g->pending_next = g->pending_count = 0;
    return true;
}

static void forward_input_event(InputEvent event) {
    if (event.reserved || !event.source || !event.generation || !event.sequence ||
        event.usage_page != 7 || event.type == INPUT_TEXT || event.type < INPUT_KEY_DOWN ||
        event.type > INPUT_SOURCE_RESET) {
        return;
    }
    Source* source = nullptr;
    Source* free = nullptr;
    for (auto& row : g->sources) {
        if (row.token == event.source) {
            source = &row;
        }
        if (!row.token && !free) {
            free = &row;
        }
    }
    if (!source) {
        source = free;
    }
    if (!source || (source->token && (event.generation < source->generation ||
                                      (event.generation == source->generation &&
                                       event.sequence <= source->sequence)))) {
        return;
    }
    // A new incarnation or missing sequence explicitly invalidates held state.
    // Consumers can release this source without disturbing another keyboard.
    const bool reset = source->token && (event.generation != source->generation ||
                                         event.sequence != source->sequence + 1);
    if (reset) {
        auto notice = event;
        notice.type = INPUT_SOURCE_RESET;
        notice.generation = source->generation;
        notice.sequence = source->sequence;
        notice.keycode = notice.modifiers = notice.unicode = 0;
        g->pending[g->pending_count++] = notice;
        libk::zero(*source);
    }
    if (!source->token) {
        source->token = event.source;
        source->generation = event.generation;
        g->leds_dirty = true;
    }
    source->sequence = event.sequence;
    source->modifiers = event.modifiers & 255u;
    bool fresh_press = false;
    if ((event.type == INPUT_KEY_DOWN || event.type == INPUT_KEY_UP) && event.keycode < 256) {
        const auto bit = static_cast<uint8_t>(1u << (event.keycode % 8));
        auto& held = source->held[event.keycode / 8];
        fresh_press = event.type == INPUT_KEY_DOWN && !(held & bit);
        if (event.type == INPUT_KEY_DOWN) {
            held |= bit;
        } else {
            held &= static_cast<uint8_t>(~bit);
        }
    }
    if (fresh_press && event.keycode < 0xe0 && event.keycode != 0x39 && event.keycode != 0x53 &&
        event.keycode != 0x47) {
        source->repeat_key = event.keycode;
        source->repeat_at = mx_clock_ns() + 500000000ull;
    } else if (event.type == INPUT_KEY_UP && event.keycode == source->repeat_key) {
        source->repeat_key = 0;
        source->repeat_at = 0;
    }
    if (event.type == INPUT_SOURCE_DETACHED || event.type == INPUT_SOURCE_RESET) {
        libk::zero(*source);
    }
    event.modifiers = 0;
    for (const auto& row : g->sources) {
        event.modifiers |= row.modifiers;
    }
    g->pending[g->pending_count++] = event;
    if (!fresh_press) {
        return;
    }
    const uint8_t lock = event.keycode == 0x39   ? 2
                         : event.keycode == 0x53 ? 1
                         : event.keycode == 0x47 ? 4
                                                 : 0;
    if (lock) {
        g->locks ^= lock;
        g->leds_dirty = true;
    }
    char character = control_for_key(event.keycode, event.modifiers);
    if (!character && !(event.modifiers &
                        (INPUT_MOD_LCTRL | INPUT_MOD_RCTRL | INPUT_MOD_LALT | INPUT_MOD_RALT))) {
        character = ascii_for_key(event.keycode, event.modifiers);
    }
    if (!character) {
        return;
    }
    // Preserve source, generation and sequence in derived text events.
    event.type = INPUT_TEXT;
    event.unicode = static_cast<uint32_t>(static_cast<unsigned char>(character));
    g->pending[g->pending_count++] = event;
}

// Repeat is input policy, independent of HID/controller polling intervals.
// A source reset or release removes its repeat row; another keyboard's held
// modifiers remain in effect. Derived events preserve producer provenance.
static bool repeat_due() {
    const auto now = mx_clock_ns();
    for (auto& source : g->sources) {
        if (source.token && source.repeat_key && now >= source.repeat_at) {
            source.repeat_at = now + 33000000;
            InputEvent event{INPUT_KEY_REPEAT,  source.repeat_key, 0, 0, source.token,
                             source.generation, source.sequence,   7, 0};
            for (const auto& row : g->sources) {
                event.modifiers |= row.modifiers;
            }
            g->pending[g->pending_count++] = event;
            char character = control_for_key(event.keycode, event.modifiers);
            if (!character && !(event.modifiers & (INPUT_MOD_LCTRL | INPUT_MOD_RCTRL |
                                                   INPUT_MOD_LALT | INPUT_MOD_RALT))) {
                character = ascii_for_key(event.keycode, event.modifiers);
            }
            if (character) {
                event.type = INPUT_TEXT;
                event.unicode = static_cast<unsigned char>(character);
                g->pending[g->pending_count++] = event;
            }
            return true;
        }
    }
    return false;
}

// Policy deadlines are independent of controller cadence. Sleep indefinitely
// when no key repeats; a producer publication wakes the common input doorbell.
static uint64_t repeat_deadline() {
    uint64_t deadline = 0;
    for (const auto& source : g->sources) {
        if (source.token && source.repeat_key) {
            deadline = libk::earlier_deadline(deadline, source.repeat_at);
        }
    }
    return deadline;
}
static bool service() {
    if (!flush_pending()) {
        return false;
    }
    if (g->leds_dirty) {
        if (!g->leds.try_send({0, g->locks, 0})) {
            return false;
        }
        g->leds_dirty = false;
    }
    InputEvent event{};
    for (uint32_t n = 0; n < 2; ++n) {
        const auto i = (g->next_reader + n) & 1;
        if ((i ? g->test : g->hid).try_read(&event)) {
            forward_input_event(event);
            g->next_reader = i ^ 1; // neither producer can starve the other
            return true;
        }
    }
    return repeat_due();
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "ui.input");

    auto arena = mochi::make_state(0x1b00'0000ull, 16384);
    g = arena.alloc<Runtime>(1);
    if (!g) {
        mochi::boot::invalid_boot();
    }
    g->hid = {mochi::boot::map_ring(boot, {"hid.input.ring", kVaHidRing, "hid.input.doorbell"})};
    g->test = {mochi::boot::map_ring(boot, {"test.input.ring", 0x1b010000, "test.input.doorbell"})};
    if (g->test.valid() && (g->test.order != g->hid.order)) {
        mochi::boot::invalid_boot();
    }
    g->readers[0].rx_ring = g->hid.ring;
    g->readers[0].order = g->hid.order;
    g->readers[1].rx_ring = g->test.ring;
    g->readers[1].order = g->test.order;
    g->tty = {mochi::boot::map_ring(boot, {"input.tty.ring", kVaTtyRing, "input.tty.doorbell"})};
    g->leds = {mochi::boot::map_ring(boot, {"hid.led.ring", kVaLedRing, "hid.led.doorbell"})};
    if (!g->hid.valid() || !g->tty.valid() || !g->leds.valid()) {
        mochi::boot::invalid_boot();
    }

    mochi::log_raw_marker("ui.input: ready\n");

    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch(service)) {
            mx_yield();
            continue;
        }
        const bool blocked = g->pending_next < g->pending_count || g->leds_dirty;
        const auto deadline =
            blocked ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : repeat_deadline();
        mochi::driver::idle(g->readers, 2, g->hid.doorbell, deadline, blocked ? 0u : 3u);
    }
}
