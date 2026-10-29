#pragma once
// Boot-keyboard class decoder. Controller code does not own input semantics.
#include "libk/bytes.h"
#include "midix/abi/protocols/usb.h"
#include "midix/abi/protocols/input.h"

namespace midix::usb {

class BootKeyboardDecoder {
public:
    bool prepare(libk::const_bytes bytes) {
        if (pending() || !bytes.data || bytes.size != sizeof(UsbBootKeyboardReport)) return false;
        UsbBootKeyboardReport report{};
        if (!libk::copy_exact(libk::as_bytes(&report, sizeof(report)), bytes)) return false;
        bool rollover = false;
        for (auto key : report.keys) if (key >= 1 && key <= 3) rollover = true;
        if (rollover) {
            // ErrorRollOver/PostFail/ErrorUndefined describe unknown key state.
            // Modifier bits remain valid; retain held ordinary keys.
            if (!libk::copy_exact(libk::as_bytes(report.keys), libk::as_bytes(previous_.keys))) return false;
        }
        target_ = report;
        count_ = next_ = 0;
        const auto changed = previous_.modifiers ^ report.modifiers;
        for (mx_u32_t bit = 0; bit < 8; ++bit) {
            if (!(changed & (1u << bit))) continue;
            add((report.modifiers & (1u << bit)) ? INPUT_KEY_DOWN : INPUT_KEY_UP, 0xe0 + bit);
        }
        for (mx_u32_t i = 0; i < 6; ++i) {
            const auto key = previous_.keys[i];
            if (ordinary(key) && !earlier(previous_.keys, i, key) && !contains(report.keys, key))
                add(INPUT_KEY_UP, key);
        }
        for (mx_u32_t i = 0; i < 6; ++i) {
            const auto key = report.keys[i];
            if (ordinary(key) && !earlier(report.keys, i, key) && !contains(previous_.keys, key))
                add(INPUT_KEY_DOWN, key);
        }
        if (!count_) previous_ = target_;
        return true;
    }
    bool pending() const { return next_ < count_; }
    const InputEvent* event() const { return pending() ? events_ + next_ : nullptr; }
    bool advance() {
        if (!pending()) return false;
        if (++next_ == count_) previous_ = target_;
        return true;
    }
    const UsbBootKeyboardReport& previous() const { return previous_; }
private:
    static bool ordinary(mx_u8_t key) { return key > 3 && (key < 0xe0 || key > 0xe7); }
    static bool earlier(const mx_u8_t* keys, mx_u32_t count, mx_u8_t key) {
        for (mx_u32_t i = 0; i < count; ++i) if (keys[i] == key) return true;
        return false;
    }
    static bool contains(const mx_u8_t* keys, mx_u8_t key) { return earlier(keys, 6, key); }
    void add(mx_u32_t type, mx_u32_t key) {
        events_[count_++] = {type, key, target_.modifiers, 0};
    }
    UsbBootKeyboardReport previous_{};
    UsbBootKeyboardReport target_{};
    InputEvent events_[20]{}; // 8 modifiers + 6 releases + 6 presses
    mx_u32_t count_ = 0, next_ = 0;
};

} // namespace midix::usb
