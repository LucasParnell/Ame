#pragma once
// mochi/log.h — small non-blocking userspace logging helper.
//
// Each producer owns one SPSC ring and shares the console doorbell.  Normal
// logging never blocks: full rings increment counters and a later successful
// write emits a compact drop notice.

#include <cstdint>
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

static inline LogSink& default_log_sink() {
    static LogSink sink{};
    return sink;
}

static inline uint32_t log_slen(const char* s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
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

static inline void log_append_char(char* out, uint32_t cap, uint32_t* n, bool* truncated, char c) {
    if (*n + 1 < cap) {
        out[(*n)++] = c;
    } else if (truncated) {
        *truncated = true;
    }
}

static inline void log_append_str(char* out, uint32_t cap, uint32_t* n,
                                  bool* truncated, const char* s) {
    if (!s) s = "(null)";
    while (*s) log_append_char(out, cap, n, truncated, *s++);
}

static inline void log_append_u64(char* out, uint32_t cap, uint32_t* n,
                                  bool* truncated, uint64_t value) {
    char tmp[24];
    uint32_t t = 0;
    do {
        tmp[t++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && t < sizeof(tmp));
    while (t) log_append_char(out, cap, n, truncated, tmp[--t]);
}

static inline void log_append_u64_zero_padded(char* out, uint32_t cap, uint32_t* n,
                                              bool* truncated, uint64_t value,
                                              uint32_t width) {
    char tmp[24];
    uint32_t t = 0;
    do {
        tmp[t++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && t < sizeof(tmp));
    while (t < width && t < sizeof(tmp)) tmp[t++] = '0';
    while (t) log_append_char(out, cap, n, truncated, tmp[--t]);
}

static inline void log_append_u64_space_padded(char* out, uint32_t cap, uint32_t* n,
                                               bool* truncated, uint64_t value,
                                               uint32_t width) {
    char tmp[24];
    uint32_t t = 0;
    do {
        tmp[t++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && t < sizeof(tmp));
    while (t < width) {
        log_append_char(out, cap, n, truncated, ' ');
        --width;
    }
    while (t) log_append_char(out, cap, n, truncated, tmp[--t]);
}

static inline void log_append_timestamp(char* out, uint32_t cap, uint32_t* n,
                                        bool* truncated) {
    int64_t ticks_raw = sys_debug_counter(7);
    uint64_t ticks = ticks_raw > 0 ? static_cast<uint64_t>(ticks_raw) : 0;
    const uint64_t sec = ticks / 100ull;
    const uint64_t usec = (ticks % 100ull) * 10000ull;

    log_append_char(out, cap, n, truncated, '[');
    log_append_u64_space_padded(out, cap, n, truncated, sec, 5);
    log_append_char(out, cap, n, truncated, '.');
    log_append_u64_zero_padded(out, cap, n, truncated, usec, 6);
    log_append_char(out, cap, n, truncated, ']');
}

static inline uint32_t log_finish(char* out, uint32_t cap, uint32_t n) {
    if (cap == 0) return 0;
    if (n >= cap) n = cap - 1;
    out[n] = '\0';
    return n;
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
    sink.ready = ring != nullptr && doorbell != kNullCap;
    log_set_sink(sink);
}

static inline bool log_has_capacity(const LogSink& sink, uint32_t chunks) {
    const uint64_t tail = __atomic_load_n(&sink.ring->tail.v, __ATOMIC_ACQUIRE);
    const uint64_t head = __atomic_load_n(&sink.ring->head.v, __ATOMIC_RELAXED);
    const uint64_t used = head - tail;
    const uint64_t capacity = 1ull << sink.order;
    return used <= capacity && chunks <= static_cast<uint32_t>(capacity - used);
}

static inline bool log_try_push_bytes(LogSink& sink, const char* data, uint32_t len) {
    if (!sink.ready || !data) return false;
    if (len == 0) return true;

    const uint32_t chunks = (len + 47u) / 48u;
    if (!log_has_capacity(sink, chunks)) return false;

    uint32_t off = 0;
    while (off < len) {
        const uint32_t chunk = (len - off) > 48u ? 48u : (len - off);
        if (!ring_push(sink.ring, sink.order, 1, data + off, chunk, sink.doorbell)) {
            return false;
        }
        off += chunk;
    }
    return true;
}

static inline void log_emit_drop_notice(LogSink& sink) {
    if (sink.emitting_notice || (sink.dropped == 0 && sink.truncated == 0)) return;
    sink.emitting_notice = true;

    char line[112];
    uint32_t n = 0;
    bool truncated = false;
    log_append_timestamp(line, sizeof(line), &n, &truncated);
    log_append_str(line, sizeof(line), &n, &truncated, " [W] ");
    log_append_str(line, sizeof(line), &n, &truncated, sink.component ? sink.component : "mochi");
    log_append_str(line, sizeof(line), &n, &truncated, ": dropped=");
    log_append_u64(line, sizeof(line), &n, &truncated, sink.dropped);
    log_append_str(line, sizeof(line), &n, &truncated, " truncated=");
    log_append_u64(line, sizeof(line), &n, &truncated, sink.truncated);
    log_append_char(line, sizeof(line), &n, &truncated, '\n');
    n = log_finish(line, sizeof(line), n);

    if (log_try_push_bytes(sink, line, n)) {
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
    uint32_t n = 0;
    bool truncated = false;
    log_append_timestamp(line, cap, &n, &truncated);
    log_append_str(line, cap, &n, &truncated, " [");
    log_append_char(line, cap, &n, &truncated, log_level_char(level));
    log_append_str(line, cap, &n, &truncated, "] ");
    log_append_str(line, cap, &n, &truncated, component ? component : "mochi");
    log_append_str(line, cap, &n, &truncated, ": ");

    if (!msg) msg = "(null)";
    uint32_t msg_len = log_slen(msg);
    if (msg_len > 192u) {
        msg_len = 192u;
        truncated = true;
    }
    for (uint32_t i = 0; i < msg_len; ++i) {
        log_append_char(line, cap, &n, &truncated, msg[i]);
    }
    if (msg_len == 0 || msg[msg_len - 1] != '\n') {
        log_append_char(line, cap, &n, &truncated, '\n');
    }
    n = log_finish(line, cap, n);
    if (out_truncated) *out_truncated = truncated;
    return n;
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
    char line[96];
    uint32_t n = 0;
    bool truncated = false;
    log_append_str(line, sizeof(line), &n, &truncated, key ? key : "metric");
    log_append_char(line, sizeof(line), &n, &truncated, '=');
    log_append_u64(line, sizeof(line), &n, &truncated, value);
    log_append_char(line, sizeof(line), &n, &truncated, '\n');
    n = log_finish(line, sizeof(line), n);
    if (truncated) ++default_log_sink().truncated;
    (void)n;
    log_write(LogLevel::Info, line);
}

static inline uint64_t log_dropped_count() {
    return default_log_sink().dropped;
}

static inline uint64_t log_truncated_count() {
    return default_log_sink().truncated;
}

} // namespace mochi
