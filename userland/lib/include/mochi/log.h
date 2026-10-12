#pragma once
// mochi/log.h — small non-blocking userspace logging helper.
//
// Each producer owns one SPSC ring and shares the console doorbell.  Normal
// logging never blocks: full rings increment counters and a later successful
// write emits a compact drop notice.

#include <cstdint>
#include "libk/printf.h"
#include "libk/str.h"
#include "libk/text.h"
#include "libk/integer.h"
#include "mochi/ring.h"

namespace mochi {

enum class LogLevel : uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

struct LogSink {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;
    const char* component;
    uint64_t dropped;
    uint64_t truncated;
    bool ready;
    bool emitting_notice;
};

inline LogSink& default_log_sink() {
    static LogSink sink{};
    return sink;
}

static inline char log_level_char(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return 'T';
    case LogLevel::Debug: return 'D';
    case LogLevel::Info:  return 'I';
    case LogLevel::Warn:  return 'W';
    case LogLevel::Error: return 'E';
    case LogLevel::Fatal: return 'F';
    }
    return '?';
}

static inline void log_append_timestamp(libk::str::Buffer& line) {
    const int64_t ticks_raw = sys_debug_counter(7);
    const uint64_t ticks = ticks_raw > 0 ? static_cast<uint64_t>(ticks_raw) : 0;
    line.format("[%5llu.%06llu]", static_cast<unsigned long long>(ticks / 100),
                static_cast<unsigned long long>((ticks % 100) * 10000));
}

static inline void log_set_sink(const LogSink& sink) {
    default_log_sink() = sink;
}

static inline void log_set_sink(RingPage* ring, uint32_t order,
                                CapHandle doorbell, const char* component) {
    LogSink sink{};
    sink.ring = ring;
    sink.order = order;
    sink.doorbell = doorbell;
    sink.component = component;
    sink.ready = ring_valid(ring, order) && doorbell != kNullCap;
    log_set_sink(sink);
}

static inline bool log_has_capacity(const LogSink& sink, uint32_t chunks) {
    if (!ring_valid(sink.ring, sink.order)) return false;
    const uint64_t tail = __atomic_load_n(&sink.ring->tail.v, __ATOMIC_ACQUIRE);
    const uint64_t head = __atomic_load_n(&sink.ring->head.v, __ATOMIC_RELAXED);
    const uint64_t used = head - tail;
    const uint64_t capacity = 1ull << sink.order;
    return used <= capacity && chunks <= static_cast<uint32_t>(capacity - used);
}

static inline bool log_try_push_bytes(LogSink& sink, const char* data, uint32_t len) {
    if (!sink.ready || !data) return false;
    if (len == 0) return true;

    const auto chunks = static_cast<uint32_t>(libk::div_round_up(len, MX_RING_INLINE_PAYLOAD));
    if (!log_has_capacity(sink, chunks)) return false;

    // Same chunking as every other byte-stream producer (mochi/ring.h).
    return ring_push_bytes(sink.ring, sink.order, 1, data, len, sink.doorbell) == len;
}

static inline void log_emit_drop_notice(LogSink& sink) {
    if (sink.emitting_notice || (sink.dropped == 0 && sink.truncated == 0)) return;
    sink.emitting_notice = true;

    char storage[112];
    libk::str::Buffer line(storage);
    log_append_timestamp(line);
    line.format(" [W] %s: dropped=%llu truncated=%llu\n",
                sink.component ? sink.component : "mochi",
                static_cast<unsigned long long>(sink.dropped),
                static_cast<unsigned long long>(sink.truncated));

    if (log_try_push_bytes(sink, line.data, static_cast<uint32_t>(line.length))) {
        sink.dropped = 0;
        sink.truncated = 0;
    }
    sink.emitting_notice = false;
}

static inline void log_emit_bytes(const char* data, uint32_t len) {
    LogSink& sink = default_log_sink();
    if (!sink.ready) return;
    log_emit_drop_notice(sink);
    if (!log_try_push_bytes(sink, data, len)) {
        ++sink.dropped;
    }
}

static inline uint32_t log_format_line(char* line, uint32_t cap, LogLevel level,
                                       const char* component, const char* msg,
                                       bool* out_truncated) {
    libk::str::Buffer out(line, cap);
    log_append_timestamp(out);
    out.format(" [%c] %s: ", log_level_char(level), component ? component : "mochi");

    constexpr uint32_t kMaxMessageLength = 192;
    if (!msg) msg = "(null)";
    const auto scanned = libk::str::length_n(msg, kMaxMessageLength + 1);
    const auto length = scanned > kMaxMessageLength ? kMaxMessageLength : scanned;
    out.append(msg, length);
    if (length == 0 || msg[length - 1] != '\n') out.append('\n');
    if (out_truncated) *out_truncated = out.truncated || scanned > kMaxMessageLength;
    return static_cast<uint32_t>(out.length);
}

static inline void log_write(LogLevel level, const char* msg) {
    LogSink& sink = default_log_sink();
    if (!sink.ready) return;

    char line[256];
    bool truncated = false;
    const uint32_t n = log_format_line(line,
                                       sizeof(line),
                                       level,
                                       sink.component,
                                       msg,
                                       &truncated);
    if (truncated) ++sink.truncated;
    log_emit_bytes(line, n);
}

static inline void log_raw_marker(const char* msg) {
    if (!msg) return;
    log_write(LogLevel::Info, msg);
}

static inline void log_counter(const char* key, uint64_t value) {
    char storage[96];
    libk::str::Buffer line(storage);
    line.format("%s=%llu\n", key ? key : "metric", static_cast<unsigned long long>(value));
    if (line.truncated) ++default_log_sink().truncated;
    log_write(LogLevel::Info, line.data);
}

static inline uint64_t log_dropped_count() {
    return default_log_sink().dropped;
}

static inline uint64_t log_truncated_count() {
    return default_log_sink().truncated;
}

} // namespace mochi
