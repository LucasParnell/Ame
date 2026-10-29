#pragma once
// Bounded HID keyboard report compilation/decoding. No IPC, allocation,
// controller state or text/layout policy. Storage belongs to the HID service.
// USB HID 1.11 sections 6.2.2 and 8; unsupported encodings fail explicitly.
#include "libk/bits.h"
#include "libk/endian.h"
#include "midix/abi/protocols/input.h"

namespace midix::usb::hid {
constexpr mx_u32_t kFields = 128, kReports = 16, kUsages = 512, kReportBytes = 4096;
enum class Kind : mx_u8_t { Input, Output, Feature };
struct Report {
    mx_u32_t bits[3];
    mx_u8_t id;
};
struct Field {
    mx_u32_t offset;
    mx_u32_t first_usage, last_usage;
    mx_i32_t logical_min, logical_max;
    mx_u16_t count, usage_index, usage_count;
    mx_u8_t width, report;
    Kind kind;
    bool variable;
};
struct Layout {
    Field fields[kFields];
    Report reports[kReports];
    mx_u32_t usages[kUsages]; // explicit array selectors, kept off the hot path
    mx_u32_t field_count, report_count, usage_count;
    bool numbered, keyboard;
};

namespace detail {
struct Globals {
    mx_u32_t page, width, count;
    mx_i32_t minimum, maximum;
    mx_u8_t report_id;
};
struct Locals {
    mx_u32_t usages[64], count, first, last;
    bool has_first, has_last;
};
inline mx_i32_t signed_item(mx_u32_t value, mx_u32_t bytes) {
    if (bytes == 1) return static_cast<mx_i32_t>(static_cast<int8_t>(value));
    if (bytes == 2) return static_cast<mx_i32_t>(static_cast<int16_t>(value));
    return static_cast<mx_i32_t>(value);
}
inline mx_u32_t usage(mx_u32_t value, mx_u32_t bytes, mx_u32_t page) {
    return bytes == 4 ? value : (page << 16) | value;
}
inline mx_u32_t usage_at(const Locals& local, mx_u32_t index) {
    if (local.count) return local.usages[index < local.count ? index : local.count - 1];
    if (!local.has_first || !local.has_last) return 0;
    const auto count = local.last - local.first;
    return local.first + (index < count ? index : count);
}
} // namespace detail

// On failure the output is unbound, including after a partially valid prefix.
// Callers must not attach a decoder until this returns true.
inline bool compile(libk::const_bytes descriptor, Layout* output) {
    if (!output) return false;
    libk::zero(*output);
    if (!descriptor.data || !descriptor.size || descriptor.size > kReportBytes) return false;
    auto& layout = *output;
    detail::Globals global{}, stack[4]{};
    detail::Locals local{};
    bool collections[8]{};
    mx_u32_t stack_count = 0, collection_count = 0;
    bool keyboard = false, ok = true;
    for (mx_u32_t offset = 0; offset < descriptor.size && ok;) {
        const auto prefix = descriptor.data[offset++];
        if (prefix == 0xfe) { ok = false; break; } // long items are unsupported
        const mx_u32_t bytes = (prefix & 3u) == 3 ? 4 : prefix & 3u;
        const auto type = (prefix >> 2) & 3u;
        const auto tag = static_cast<mx_u32_t>(prefix >> 4);
        if (bytes > descriptor.size - offset) { ok = false; break; }
        mx_u32_t value = 0;
        if (bytes == 1) value = descriptor.data[offset];
        else if (bytes == 2) value = libk::load_le<mx_u16_t>(descriptor.data + offset);
        else if (bytes == 4) value = libk::load_le<mx_u32_t>(descriptor.data + offset);
        offset += bytes;
        if (type == 1) {
            switch (tag) {
            case 0: if (value > 0xffff) ok = false; else global.page = value; break;
            case 1: global.minimum = detail::signed_item(value, bytes); break;
            case 2:
                if (global.minimum >= 0 && value > 0x7fffffffu) ok = false;
                else global.maximum = global.minimum < 0 ? detail::signed_item(value, bytes) : static_cast<mx_i32_t>(value);
                break;
            case 7: global.width = value; break;
            case 8:
                if (!value || value > 255) ok = false;
                else { global.report_id = static_cast<mx_u8_t>(value); layout.numbered = true; }
                break;
            case 9: global.count = value; break;
            case 10: if (bytes || stack_count == 4) ok = false; else stack[stack_count++] = global; break;
            case 11: if (bytes || !stack_count) ok = false; else global = stack[--stack_count]; break;
            default: break; // physical ranges/units do not change keyboard bits
            }
        } else if (type == 2) {
            const auto resolved = detail::usage(value, bytes, global.page);
            if (tag == 0) {
                if (local.count == 64) ok = false;
                else local.usages[local.count++] = resolved;
            } else if (tag == 1) { local.first = resolved; local.has_first = true; }
            else if (tag == 2) { local.last = resolved; local.has_last = true; }
            else if (tag == 10) ok = false; // alternative usage sets need separate semantics
        } else if (type == 0) {
            if ((local.count && local.has_first) || local.has_first != local.has_last || (local.has_first &&
                (local.last < local.first || (local.first >> 16) != (local.last >> 16)))) { ok = false; break; }
            if (tag == 10) {
                if (bytes != 1 || collection_count == 8) { ok = false; break; }
                collections[collection_count++] = keyboard;
                const auto collection_usage = detail::usage_at(local, 0);
                if (value == 1 && collection_usage == 0x00010006) keyboard = layout.keyboard = true;
            } else if (tag == 12) {
                if (bytes || !collection_count) { ok = false; break; }
                keyboard = collections[--collection_count];
            } else if (tag == 8 || tag == 9 || tag == 11) {
                if (!collection_count || !bytes || !global.width || global.width > 32 ||
                    !global.count || global.count > 256 || global.maximum < global.minimum) { ok = false; break; }
                const Kind kind = tag == 8 ? Kind::Input : tag == 9 ? Kind::Output : Kind::Feature;
                const auto kind_index = static_cast<mx_u32_t>(kind);
                mx_u32_t report_index = 0;
                while (report_index < layout.report_count && layout.reports[report_index].id != global.report_id) ++report_index;
                if (report_index == layout.report_count) {
                    if (layout.report_count == kReports) { ok = false; break; }
                    layout.reports[layout.report_count++].id = global.report_id;
                }
                auto& report = layout.reports[report_index];
                const auto field_bits = global.width * global.count;
                const auto bit_limit = (kReportBytes - (global.report_id ? 1 : 0)) * 8;
                if (report.bits[kind_index] > bit_limit || field_bits > bit_limit - report.bits[kind_index]) { ok = false; break; }
                const auto bit_offset = report.bits[kind_index];
                report.bits[kind_index] += field_bits;
                const bool variable = (value & 2u) != 0;
                if (keyboard && !(value & 1u) && kind != Kind::Feature) {
                    // Relative, buffered-byte and vendor keyboard encodings are
                    // unsupported; constant padding still contributes offsets.
                    if (value & (4u | 0x100u)) { ok = false; break; }
                    const auto fields = variable ? global.count : 1;
                    if (fields > kFields - layout.field_count) { ok = false; break; }
                    mx_u32_t usage_index = layout.usage_count;
                    if (!variable && local.count) {
                        if (local.count > kUsages - layout.usage_count) { ok = false; break; }
                        for (mx_u32_t u = 0; u < local.count; ++u) layout.usages[layout.usage_count++] = local.usages[u];
                    }
                    for (mx_u32_t f = 0; f < fields; ++f) {
                        const auto first = variable ? detail::usage_at(local, f) : local.first;
                        const auto last = variable ? first : local.last;
                        if ((!local.count && !local.has_first) || (first >> 16) > 0xffff) { ok = false; break; }
                        layout.fields[layout.field_count++] = {bit_offset + (variable ? f * global.width : 0),
                            first, last, global.minimum, global.maximum,
                            static_cast<mx_u16_t>(variable ? 1 : global.count),
                            static_cast<mx_u16_t>(usage_index), static_cast<mx_u16_t>(variable ? 0 : local.count),
                            static_cast<mx_u8_t>(global.width), static_cast<mx_u8_t>(report_index), kind, variable};
                    }
                }
            }
            libk::zero(local); // local items end at EVERY main item
        }
    }
    bool keyboard_input = false;
    for (mx_u32_t i = 0; i < layout.field_count; ++i) {
        const auto& field = layout.fields[i];
        if (field.kind != Kind::Input) continue;
        if ((field.first_usage >> 16) == 7) keyboard_input = true;
        for (mx_u32_t u = 0; u < field.usage_count; ++u)
            if ((layout.usages[field.usage_index + u] >> 16) == 7) keyboard_input = true;
    }
    if (collection_count || stack_count || !layout.keyboard || !keyboard_input) ok = false;
    if (layout.numbered)
        for (mx_u32_t i = 0; i < layout.report_count; ++i) if (!layout.reports[i].id) ok = false;
    if (!ok) libk::zero(layout);
    return ok;
}

inline mx_u32_t report_bytes(const Report& report, Kind kind) {
    const auto bits = report.bits[static_cast<mx_u32_t>(kind)];
    return (bits + 7) / 8 + (bits && report.id ? 1 : 0);
}
inline bool pressed(const mx_u8_t* keys, mx_u32_t usage) {
    return usage < 256 && (keys[usage / 8] & (1u << (usage % 8)));
}
inline void press(mx_u8_t* keys, mx_u32_t usage) {
    if (usage > 3 && usage < 256) keys[usage / 8] |= static_cast<mx_u8_t>(1u << (usage % 8));
}

class KeyboardDecoder {
public:
    bool init(const Layout* layout) {
        if (layout_ || !layout || !layout->keyboard || !layout->report_count) return false;
        layout_ = layout;
        return true;
    }
    bool prepare(libk::const_bytes bytes) {
        if (!layout_ || pending_ || !bytes.data || !bytes.size) return false;
        const auto id = layout_->numbered ? bytes.data[0] : 0;
        mx_u32_t report = 0;
        while (report < layout_->report_count && layout_->reports[report].id != id) ++report;
        if (report == layout_->report_count || bytes.size != report_bytes(layout_->reports[report], Kind::Input)) return false;
        const auto payload = libk::slice(bytes, layout_->numbered ? 1 : 0, bytes.size - (layout_->numbered ? 1 : 0));
        libk::zero(target_row_);
        bool rollover = false;
        for (mx_u32_t i = 0; i < layout_->field_count; ++i) {
            const auto& field = layout_->fields[i];
            if (field.report != report || field.kind != Kind::Input) continue;
            for (mx_u32_t n = 0; n < field.count; ++n) {
                mx_u64_t value = 0;
                if (!libk::read_bits(payload, field.offset + n * field.width, field.width, &value)) return false;
                mx_u32_t usage = field.first_usage;
                if (field.variable) { if (!value) continue; }
                else {
                    // Keyboard arrays use selector minus logical minimum to
                    // index either explicit usages or their advertised range.
                    if (field.logical_min < 0 || value < static_cast<mx_u32_t>(field.logical_min) ||
                        value > static_cast<mx_u32_t>(field.logical_max)) continue; // null selector
                    const auto selector = static_cast<mx_u32_t>(value) - field.logical_min;
                    if (field.usage_count) {
                        if (selector >= field.usage_count) continue;
                        usage = layout_->usages[field.usage_index + selector];
                    } else {
                        if (selector > field.last_usage - field.first_usage) continue;
                        usage += selector;
                    }
                }
                if ((usage >> 16) != 7) continue;
                const auto key = usage & 0xffff;
                if (key >= 1 && key <= 3) rollover = true;
                else press(target_row_, key);
            }
        }
        if (rollover)
            for (mx_u32_t key = 4; key < 256; ++key)
                if ((key < 0xe0 || key > 0xe7) && pressed(rows_[report], key)) press(target_row_, key);
        aggregate(before_, report, false);
        aggregate(after_, report, true);
        report_ = report;
        scan_ = 0;
        releasing_ = false;
        pending_ = true;
        select_event();
        return true;
    }
    bool release_all() {
        if (pending_ || !layout_) return false;
        aggregate(before_, 0, false);
        libk::zero(after_);
        scan_ = 0;
        releasing_ = pending_ = true;
        select_event();
        return true;
    }
    const InputEvent* event() const { return pending_ ? &event_ : nullptr; }
    bool advance() {
        if (!pending_) return false;
        ++scan_;
        select_event();
        return true;
    }
private:
    void aggregate(mx_u8_t* output, mx_u32_t report, bool replace) {
        libk::zero(output, 32);
        for (mx_u32_t r = 0; r < layout_->report_count; ++r)
            for (mx_u32_t b = 0; b < 32; ++b) output[b] |= replace && r == report ? target_row_[b] : rows_[r][b];
    }
    void select_event() {
        for (; scan_ < 256; ++scan_) {
            const auto down = pressed(after_, scan_);
            if (pressed(before_, scan_) == down) continue;
            mx_u32_t modifiers = 0;
            for (mx_u32_t m = 0; m < 8; ++m) if (pressed(after_, 0xe0 + m)) modifiers |= 1u << m;
            event_ = {down ? INPUT_KEY_DOWN : INPUT_KEY_UP, scan_, modifiers, 0};
            return;
        }
        // Commit only after the caller consumed every event. Backpressure may
        // pause anywhere without losing a release or decoding the report twice.
        if (releasing_) libk::zero(rows_);
        else libk::copy_exact(libk::as_bytes(rows_[report_]), libk::as_bytes(target_row_));
        pending_ = false;
    }
    const Layout* layout_ = nullptr;
    mx_u8_t rows_[kReports][32]{}, target_row_[32]{}, before_[32]{}, after_[32]{};
    InputEvent event_{};
    mx_u32_t report_ = 0, scan_ = 0;
    bool pending_ = false, releasing_ = false;
};

// Emit LED usages 1..5 in a chosen output report, keeping constant bits zero.
// The caller selects interrupt-OUT or SET_REPORT and retains the slice until
// that ordinary USB transfer completes. No hardware shortcuts are involved.
inline bool encode_leds(const Layout& layout, mx_u32_t report_index, mx_u8_t mask,
                        libk::bytes destination, mx_u32_t* length) {
    if (!length || report_index >= layout.report_count) return false;
    const auto& report = layout.reports[report_index];
    const auto bytes = report_bytes(report, Kind::Output);
    if (!bytes || !destination.data || bytes > destination.size) return false;
    bool found = false;
    for (mx_u32_t i = 0; i < layout.field_count; ++i) {
        const auto& field = layout.fields[i];
        if (field.report == report_index && field.kind == Kind::Output && field.variable &&
            field.first_usage >= 0x00080001 && field.first_usage <= 0x00080005) found = true;
    }
    if (!found) return false;
    auto output = libk::slice(destination, 0, bytes);
    libk::zero(output);
    if (report.id) output.data[0] = report.id;
    auto payload = libk::slice(output, report.id ? 1 : 0, bytes - (report.id ? 1 : 0));
    for (mx_u32_t i = 0; i < layout.field_count; ++i) {
        const auto& field = layout.fields[i];
        if (field.report != report_index || field.kind != Kind::Output || !field.variable ||
            field.first_usage < 0x00080001 || field.first_usage > 0x00080005) continue;
        if (!libk::write_bits(payload, field.offset, field.width,
            (mask >> ((field.first_usage & 0xffff) - 1)) & 1u)) return false;
    }
    *length = bytes;
    return true;
}
} // namespace midix::usb::hid
