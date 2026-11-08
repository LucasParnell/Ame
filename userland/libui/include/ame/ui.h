#pragma once
#include <stdint.h>
#include <stddef.h>

// Ame's QML runtime. No Qt, parser, global heap or graphics handles in controls.
namespace ame::ui {
constexpr uint32_t kAbiVersion = 1;
constexpr int32_t kMaxExtent = 1 << 20;
inline int32_t bounded(int64_t n) {
    return n < -2147483647LL - 1 ? (-2147483647 - 1) :
           n > 2147483647 ? 2147483647 : static_cast<int32_t>(n);
}
inline int32_t add(int32_t a, int32_t b) { return bounded(int64_t{a} + b); }
inline int32_t sub(int32_t a, int32_t b) { return bounded(int64_t{a} - b); }
inline int32_t mul(int32_t a, int32_t b) { return bounded(int64_t{a} * b); }
inline int32_t extent(int32_t n) { return n < 0 ? 0 : n > kMaxExtent ? kMaxExtent : n; }
inline int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }

struct Text {
    static constexpr uint32_t capacity = 256;
    char bytes[capacity]{};
    uint32_t size = 0;
    Text() = default;
    Text(const char* value) {
        if (value) while (size + 1 < capacity && value[size]) {
            bytes[size] = value[size]; ++size;
        }
    }
};
inline bool operator==(const Text& a, const Text& b) {
    if (a.size != b.size) return false;
    for (uint32_t i = 0; i < a.size; ++i) if (a.bytes[i] != b.bytes[i]) return false;
    return true;
}
inline bool operator!=(const Text& a, const Text& b) { return !(a == b); }
inline Text text(const Text& value) { return value; }
inline Text text(bool value) { return Text(value ? "true" : "false"); }
inline Text text(int32_t value) {
    Text out;
    uint32_t n = value < 0 ? static_cast<uint32_t>(-int64_t{value}) : value;
    char digits[10]; uint32_t count = 0;
    do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
    if (value < 0) out.bytes[out.size++] = '-';
    while (count) out.bytes[out.size++] = digits[--count];
    return out;
}
inline Text concat(Text a, const Text& b) {
    for (uint32_t i = 0; i < b.size && a.size + 1 < Text::capacity; ++i)
        a.bytes[a.size++] = b.bytes[i];
    a.bytes[a.size] = 0;
    return a;
}
struct Rect {
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool contains(int32_t px, int32_t py) const {
        return width > 0 && height > 0 && px >= x && py >= y &&
               int64_t{px} < int64_t{x} + width && int64_t{py} < int64_t{y} + height;
    }
};
inline Rect intersect(Rect a, Rect b) {
    const auto x = maximum(a.x, b.x), y = maximum(a.y, b.y);
    return {x, y, maximum(0, sub(minimum(add(a.x, a.width), add(b.x, b.width)), x)),
                  maximum(0, sub(minimum(add(a.y, a.height), add(b.y, b.height)), y))};
}
enum class Kind : uint8_t { Item, Row, Column, Rectangle, Text, Button, MouseArea };
// AnchorCenterIn carries a sibling index, kAnchorParent, or kAnchorNone.
constexpr int32_t kAnchorNone = -1, kAnchorParent = -2;
enum class Property : uint8_t { X, Y, Z, Width, Height, Spacing, Padding, Color, Visible, Enabled, Clip, Fill,
                                AnchorCenterIn, AnchorMargins };
enum class LayoutError : uint8_t { None, InvalidTree, CyclicSizing };
struct Node {
    Kind kind = Kind::Item;
    int32_t parent = -1;
    int32_t x = 0, y = 0, z = 0, width = -1, height = -1, spacing = 0, padding = 0;
    int32_t anchor_center = kAnchorNone, anchor_margins = 0;
    uint32_t color = 0xffffffff;
    bool visible = true, enabled = true, clip = false, fill = false;
    Text label{};
};
struct Geometry { Rect box{}, clip{}; bool visible = false, enabled = false; };

// Fixed component storage: caller chooses capacity, generated components use
// exactly their static node count. Dynamic/keyed components will own separate
// instances rather than move live state or graphics resources in this array.
template<size_t Capacity> class Scene {
    static_assert(Capacity > 0 && Capacity <= 1024);
    Node nodes_[Capacity]{};
    Geometry committed_[Capacity]{}, pending_[Capacity]{};
    int32_t measured_width_[Capacity]{}, measured_height_[Capacity]{};
    // Painting and hit testing share one order: parent, then its children
    // sorted by (z, declaration). Children therefore stay above the parent.
    uint32_t order_[Capacity]{}, order_count_ = 0;
    uint32_t scratch_[Capacity]{}, scratch_used_ = 0;
    uint32_t count_ = 0;
    int32_t viewport_width_ = -1, viewport_height_ = -1, capture_ = -1, focus_ = -1;
    bool layout_dirty_ = true, paint_dirty_ = true, ready_ = false;
    uint64_t layouts_ = 0, frames_ = 0;
    LayoutError error_ = LayoutError::None;
    bool measure(uint32_t index, int32_t available_width, int32_t available_height) {
        const auto& n = nodes_[index];
        const auto width = n.fill ? available_width : n.width;
        const auto height = n.fill ? available_height : n.height;
        const auto pad = extent(n.padding);
        const auto inner_width = maximum(0, (width < 0 ? available_width : extent(width)) - 2 * pad);
        const auto inner_height = maximum(0, (height < 0 ? available_height : extent(height)) - 2 * pad);
        int32_t content_width = 0, content_height = 0, children = 0;
        if (n.kind == Kind::Text || n.kind == Kind::Button) {
            // Bootstrap ASCII metrics; shaping/fallback replaces this in C4.
            content_width = static_cast<int32_t>(n.label.size) * 8;
            content_height = 16;
        }
        for (uint32_t i = index + 1; i < count_; ++i) if (nodes_[i].parent == int32_t(index) && nodes_[i].visible) {
            if (nodes_[i].fill && (width < 0 || height < 0)) {
                error_ = LayoutError::CyclicSizing; return false;
            }
            if (!measure(i, inner_width, inner_height)) return false;
            const auto cw = measured_width_[i], ch = measured_height_[i];
            if (n.kind == Kind::Row) {
                content_width = extent(add(content_width, cw));
                content_height = maximum(content_height, ch);
            } else if (n.kind == Kind::Column) {
                content_width = maximum(content_width, cw);
                content_height = extent(add(content_height, ch));
            } else {
                content_width = maximum(content_width, extent(add(nodes_[i].x, cw)));
                content_height = maximum(content_height, extent(add(nodes_[i].y, ch)));
            }
            ++children;
        }
        const auto gaps = children > 1 ? extent(mul(children - 1, extent(n.spacing))) : 0;
        if (n.kind == Kind::Row) content_width = extent(add(content_width, gaps));
        if (n.kind == Kind::Column) content_height = extent(add(content_height, gaps));
        measured_width_[index] = width < 0 ? extent(add(content_width, 2 * pad)) : extent(width);
        measured_height_[index] = height < 0 ? extent(add(content_height, 2 * pad)) : extent(height);
        return true;
    }
    // `content` is the parent's padded content box, also the reference for an
    // anchor to the parent. A sibling anchor instead uses that sibling's
    // committed-in-this-pass box, so an earlier declaration is required.
    void arrange(uint32_t index, Rect content, Rect clip, bool visible, bool enabled) {
        const auto& n = nodes_[index];
        auto& geometry = pending_[index];
        const auto width = measured_width_[index], height = measured_height_[index];
        geometry.box = {add(content.x, n.x), add(content.y, n.y), width, height};
        const Rect* reference = nullptr;
        // fill without centerIn still resolves against the parent.
        if (n.anchor_center == kAnchorParent || (n.fill && n.anchor_center == kAnchorNone))
            reference = &content;
        else if (n.anchor_center >= 0 && uint32_t(n.anchor_center) < index)
            reference = &pending_[uint32_t(n.anchor_center)].box;
        if (reference) {
            const auto margin = n.anchor_margins;
            if (n.fill) {
                geometry.box = {add(reference->x, margin), add(reference->y, margin),
                                maximum(0, sub(reference->width, 2 * margin)),
                                maximum(0, sub(reference->height, 2 * margin))};
            } else {
                // A positive margin grows the item outward from the anchor
                // lines, matching Qt's outside-of-the-anchor semantics.
                geometry.box = {sub(add(reference->x, (reference->width - width) / 2), margin),
                                sub(add(reference->y, (reference->height - height) / 2), margin),
                                maximum(0, add(width, 2 * margin)),
                                maximum(0, add(height, 2 * margin))};
            }
        }
        geometry.clip = clip;
        geometry.visible = visible && n.visible;
        geometry.enabled = enabled && n.enabled;
        const auto child_clip = n.clip ? intersect(clip, geometry.box) : clip;
        const auto pad = extent(n.padding);
        int32_t cx = add(geometry.box.x, pad), cy = add(geometry.box.y, pad);
        const auto inner_width = maximum(0, sub(geometry.box.width, 2 * pad));
        const auto inner_height = maximum(0, sub(geometry.box.height, 2 * pad));
        for (uint32_t i = index + 1; i < count_; ++i) if (nodes_[i].parent == int32_t(index)) {
            arrange(i, {cx, cy, inner_width, inner_height}, child_clip, geometry.visible, geometry.enabled);
            if (nodes_[i].visible) {
                if (n.kind == Kind::Row) cx = add(cx, add(measured_width_[i], extent(n.spacing)));
                if (n.kind == Kind::Column) cy = add(cy, add(measured_height_[i], extent(n.spacing)));
            }
        }
    }
    // Depth first: a node, then its own sorted children. Appending a subtree
    // writes past `end`, so the sorted sibling slots stay readable.
    // Children are gathered into a scratch slice so recursion keeps its own.
    void append_subtree(uint32_t index) {
        order_[order_count_++] = index;
        const auto first = scratch_used_;
        for (uint32_t i = index + 1; i < count_; ++i)
            if (nodes_[i].parent == int32_t(index)) scratch_[scratch_used_++] = i;
        for (auto i = first + 1; i < scratch_used_; ++i) {
            const auto key = scratch_[i];
            auto j = i;
            while (j > first && nodes_[key].z < nodes_[scratch_[j - 1]].z) {
                scratch_[j] = scratch_[j - 1]; --j;
            }
            scratch_[j] = key;
        }
        const auto last = scratch_used_;
        for (auto i = first; i < last; ++i) append_subtree(scratch_[i]);
        scratch_used_ = first;
    }
    bool interactive(uint32_t i) const {
        const auto bounds = intersect(committed_[i].box, committed_[i].clip);
        return committed_[i].visible && committed_[i].enabled &&
               bounds.width > 0 && bounds.height > 0 &&
               (nodes_[i].kind == Kind::Button || nodes_[i].kind == Kind::MouseArea);
    }
  public:
    bool add_node(Kind kind, int32_t parent) {
        if (count_ == Capacity || (!count_ && parent != -1) ||
            (count_ && (parent < 0 || parent >= int32_t(count_)))) return false;
        uint32_t depth = 0;
        for (auto ancestor = parent; ancestor >= 0; ancestor = nodes_[ancestor].parent)
            if (++depth >= 12) return false;
        nodes_[count_].kind = kind; nodes_[count_++].parent = parent;
        invalidate(); return true;
    }
    void invalidate() { layout_dirty_ = paint_dirty_ = true; }
    uint32_t count() const { return count_; }
    const Node& node(uint32_t i) const { return nodes_[i]; }
    const Geometry& geometry(uint32_t i) const { return committed_[i]; }
    bool set(uint32_t i, Property property, int32_t value) {
        if (i >= count_) return false;
        auto& n = nodes_[i]; bool changed = false;
        auto number = [&](int32_t& target) { changed = target != value; target = value; };
        auto flag = [&](bool& target) { changed = target != bool(value); target = bool(value); };
        switch (property) {
        case Property::X: number(n.x); break;
        case Property::Y: number(n.y); break;
        case Property::Z: number(n.z); break;
        case Property::Width: number(n.width); break;
        case Property::Height: number(n.height); break;
        case Property::Spacing: number(n.spacing); break;
        case Property::Padding: number(n.padding); break;
        case Property::Color: changed = n.color != uint32_t(value); n.color = uint32_t(value); break;
        case Property::Visible: flag(n.visible); break;
        case Property::Enabled: flag(n.enabled); break;
        case Property::Clip: flag(n.clip); break;
        case Property::Fill: flag(n.fill); break;
        case Property::AnchorCenterIn: number(n.anchor_center); break;
        case Property::AnchorMargins: number(n.anchor_margins); break;
        }
        if (changed) invalidate();
        return changed;
    }
    bool set_text(uint32_t i, const Text& value) {
        if (i >= count_ || nodes_[i].label == value) return false;
        nodes_[i].label = value; invalidate(); return true;
    }
    bool layout(int32_t width, int32_t height) {
        width = extent(width); height = extent(height);
        if (width != viewport_width_ || height != viewport_height_) invalidate();
        if (!layout_dirty_) return true;
        error_ = LayoutError::None;
        if (!count_) { error_ = LayoutError::InvalidTree; return false; }
        if (!measure(0, width, height)) return false;
        arrange(0, {0, 0, width, height}, {0, 0, width, height}, true, true);
        for (uint32_t i = 0; i < count_; ++i) committed_[i] = pending_[i];
        order_count_ = scratch_used_ = 0; append_subtree(0);
        viewport_width_ = width; viewport_height_ = height;
        ready_ = true; layout_dirty_ = false; ++layouts_;
        if (capture_ >= 0 && !interactive(capture_)) capture_ = -1;
        if (focus_ >= 0 && !interactive(focus_)) focus_ = -1;
        return true;
    }
    LayoutError error() const { return error_; }
    bool needs_frame() const { return ready_ && !layout_dirty_ && paint_dirty_; }
    uint64_t layouts() const { return layouts_; }
    uint64_t frames() const { return frames_; }
    int32_t hit_test(int32_t x, int32_t y) const {
        if (!ready_) return -1;
        for (uint32_t i = order_count_; i; --i) {
            const auto index = order_[i - 1];
            if (interactive(index) && committed_[index].box.contains(x, y) &&
                committed_[index].clip.contains(x, y)) return index;
        }
        return -1;
    }
    void pointer_down(int32_t x, int32_t y) { capture_ = hit_test(x, y); focus_ = capture_; }
    int32_t pointer_up(int32_t x, int32_t y) {
        const auto target = capture_; capture_ = -1;
        return target >= 0 && target == hit_test(x, y) ? target : -1;
    }
    void cancel_input() { capture_ = -1; }
    int32_t focus_next() {
        if (!ready_) return -1;
        for (uint32_t step = 1; step <= count_; ++step) {
            const auto i = uint32_t(focus_ + int32_t(step)) % count_;
            if (interactive(i)) { focus_ = i; return focus_; }
        }
        return focus_ = -1;
    }
    int32_t activate_focus() const { return focus_ >= 0 && interactive(focus_) ? focus_ : -1; }
    // A sink supplies rectangle(Rect, clip, ARGB) and text(Rect, clip, Text,
    // ARGB), returning false on failure. Only successful presentation consumes
    // invalidation; a failed backend can retry without losing the frame.
    template<class Sink> bool draw(Sink& sink) const {
        if (!needs_frame()) return false;
        for (uint32_t k = 0; k < order_count_; ++k) {
            const auto i = order_[k];
            const auto& n = nodes_[i]; const auto& g = committed_[i];
            if (!g.visible || !intersect(g.box, g.clip).width || !intersect(g.box, g.clip).height) continue;
            if ((n.kind == Kind::Rectangle || n.kind == Kind::Button) &&
                !sink.rectangle(g.box, g.clip, n.color)) return false;
            if ((n.kind == Kind::Text || n.kind == Kind::Button) &&
                !sink.text(g.box, g.clip, n.label, n.kind == Kind::Button ? 0xffffffff : n.color)) return false;
        }
        return true;
    }
    void presented() { if (needs_frame()) { paint_dirty_ = false; ++frames_; } }
};
} // namespace ame::ui
