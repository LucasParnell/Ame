#include "anko/log.h"
#include "anko/ipc.h"
#include "anko/sched.h"
#include "libk/string.h"

namespace drivers::uart { void putchar(char c); }

namespace anko::log {

namespace {

constexpr u32 kLogSlotType = 1;
constexpr usize kMaxLine = 256;
constexpr usize kMaxMessage = 192;

Ring* g_rings[kMaxCpus]{};
Doorbell* g_doorbell = nullptr;
u32 g_ring_count = 0;
bool g_uart_ready = false;
u64 g_boot_counter = 0;
u64 g_counter_freq = 100000000;
u32 g_in_log[kMaxCpus]{};
u64 g_dropped[kMaxCpus]{};
u64 g_truncated[kMaxCpus]{};

u32 cpu_index() {
    u64 mpidr = 0;
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    const u32 id = static_cast<u32>(mpidr & 0xFFu);
    return id < kMaxCpus ? id : 0;
}

u64 read_counter() {
    u64 value = 0;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(value));
    return value;
}

u64 read_counter_freq() {
    u64 value = 0;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(value));
    return value ? value : 100000000ull;
}

char level_char(Level level) {
    switch (level) {
    case Level::Trace: return 'T';
    case Level::Debug: return 'D';
    case Level::Info:  return 'I';
    case Level::Warn:  return 'W';
    case Level::Error: return 'E';
    case Level::Fatal: return 'F';
    }
    return '?';
}

void uart_write(const char* s, usize len) {
    if (!g_uart_ready || !s) return;
    for (usize i = 0; i < len; ++i) drivers::uart::putchar(s[i]);
}

struct Writer {
    char* out;
    usize cap;
    usize len;
    bool truncated;
};

void append_char(Writer& w, char c) {
    if (w.len + 1 < w.cap) {
        w.out[w.len++] = c;
    } else {
        w.truncated = true;
    }
}

void append_str(Writer& w, const char* s) {
    if (!s) s = "(null)";
    while (*s) append_char(w, *s++);
}

void append_u64(Writer& w, u64 value, u32 base, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    u32 n = 0;
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value && n < sizeof(tmp));
    while (n) append_char(w, tmp[--n]);
}

void append_u64_padded(Writer& w, u64 value, u32 width) {
    char tmp[32];
    u32 n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && n < sizeof(tmp));
    while (n < width && n < sizeof(tmp)) tmp[n++] = '0';
    while (n) append_char(w, tmp[--n]);
}

void append_u64_space_padded(Writer& w, u64 value, u32 width) {
    char tmp[32];
    u32 n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && n < sizeof(tmp));
    while (n < width) {
        append_char(w, ' ');
        --width;
    }
    while (n) append_char(w, tmp[--n]);
}

void append_timestamp(Writer& w) {
    const u64 now = read_counter();
    const u64 delta = now >= g_boot_counter ? (now - g_boot_counter) : 0;
    const u64 freq = g_counter_freq ? g_counter_freq : 100000000ull;
    const u64 sec = delta / freq;
    const u64 rem = delta % freq;
    const u64 usec = (rem * 1000000ull) / freq;

    append_char(w, '[');
    append_u64_space_padded(w, sec, 5);
    append_char(w, '.');
    append_u64_padded(w, usec, 6);
    append_char(w, ']');
}

void append_i64(Writer& w, i64 value) {
    if (value < 0) {
        append_char(w, '-');
        const u64 mag = static_cast<u64>(-(value + 1)) + 1;
        append_u64(w, mag, 10, false);
        return;
    }
    append_u64(w, static_cast<u64>(value), 10, false);
}

usize finish(Writer& w) {
    if (w.cap == 0) return 0;
    if (w.len >= w.cap) w.len = w.cap - 1;
    w.out[w.len] = '\0';
    return w.len;
}

usize vformat(char* out, usize cap, const char* fmt, va_list ap, bool* truncated) {
    Writer w{out, cap, 0, false};
    if (!fmt) fmt = "(null)";
    while (*fmt) {
        const char c = *fmt++;
        if (c != '%') {
            append_char(w, c);
            continue;
        }

        int lenmod = 0;
        if (*fmt == 'l') {
            ++fmt;
            lenmod = 1;
            if (*fmt == 'l') {
                ++fmt;
                lenmod = 2;
            }
        } else if (*fmt == 'z') {
            ++fmt;
            lenmod = 3;
        }

        const char conv = *fmt ? *fmt++ : '\0';
        switch (conv) {
        case '%':
            append_char(w, '%');
            break;
        case 'c':
            append_char(w, static_cast<char>(va_arg(ap, int)));
            break;
        case 's':
            append_str(w, va_arg(ap, const char*));
            break;
        case 'd':
        case 'i':
            if (lenmod >= 2) append_i64(w, va_arg(ap, long long));
            else if (lenmod == 1) append_i64(w, va_arg(ap, long));
            else append_i64(w, va_arg(ap, int));
            break;
        case 'u':
            if (lenmod >= 2) append_u64(w, va_arg(ap, unsigned long long), 10, false);
            else if (lenmod == 1) append_u64(w, va_arg(ap, unsigned long), 10, false);
            else if (lenmod == 3) append_u64(w, va_arg(ap, usize), 10, false);
            else append_u64(w, va_arg(ap, unsigned int), 10, false);
            break;
        case 'x':
        case 'X':
            if (lenmod >= 2) append_u64(w, va_arg(ap, unsigned long long), 16, conv == 'X');
            else if (lenmod == 1) append_u64(w, va_arg(ap, unsigned long), 16, conv == 'X');
            else if (lenmod == 3) append_u64(w, va_arg(ap, usize), 16, conv == 'X');
            else append_u64(w, va_arg(ap, unsigned int), 16, conv == 'X');
            break;
        case 'p':
            append_str(w, "0x");
            append_u64(w, reinterpret_cast<u64>(va_arg(ap, void*)), 16, false);
            break;
        default:
            append_char(w, '%');
            if (conv) append_char(w, conv);
            break;
        }
    }
    const usize len = finish(w);
    if (truncated) *truncated = w.truncated;
    return len;
}

bool ring_has_capacity(RingHeader* header, u32 order, usize chunks) {
    const u64 tail = __atomic_load_n(&header->tail.tail, __ATOMIC_ACQUIRE);
    const u64 head = __atomic_load_n(&header->head.head, __ATOMIC_RELAXED);
    const u64 used = head - tail;
    const u64 capacity = 1ull << order;
    return used <= capacity && chunks <= static_cast<usize>(capacity - used);
}

RingSlot* ring_slots(RingHeader* header) {
    return reinterpret_cast<RingSlot*>(reinterpret_cast<u8*>(header) + sizeof(RingHeader));
}

bool push_bytes(Ring* ring, const char* data, usize len) {
    if (!ring || !data) return false;
    if (len == 0) return true;

    RingHeader* header = reinterpret_cast<RingHeader*>(ring->phys_base);
    constexpr usize kPayloadSize = 48;
    const usize chunks = (len + kPayloadSize - 1) / kPayloadSize;
    if (!ring_has_capacity(header, ring->order, chunks)) return false;

    const u64 mask = (1ull << ring->order) - 1ull;
    u64 head = __atomic_load_n(&header->head.head, __ATOMIC_RELAXED);
    usize off = 0;
    while (off < len) {
        const usize chunk = (len - off) > kPayloadSize
                                ? kPayloadSize
                                : (len - off);
        RingSlot* slot = ring_slots(header) + (head & mask);
        slot->type = kLogSlotType;
        slot->length = static_cast<u32>(chunk);
        slot->seq = head;
        for (usize i = 0; i < chunk; ++i) {
            slot->payload[i] = static_cast<u8>(data[off + i]);
        }
        __atomic_store_n(&header->head.head, head + 1, __ATOMIC_RELEASE);
        ++head;
        off += chunk;
    }

    const u32 claimed_sleep =
        __atomic_exchange_n(&header->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL);
    if (claimed_sleep && g_doorbell) {
        (void)ipc::notify_object(g_doorbell);
    }
    return true;
}

bool console_attached_for(u32 cpu) {
    return g_doorbell && cpu < g_ring_count && g_rings[cpu] != nullptr;
}

void emit_drop_notice(u32 cpu) {
    if (!console_attached_for(cpu)) return;
    const u64 dropped = __atomic_exchange_n(&g_dropped[cpu], 0ull, __ATOMIC_ACQ_REL);
    const u64 truncated = __atomic_exchange_n(&g_truncated[cpu], 0ull, __ATOMIC_ACQ_REL);
    if (dropped == 0 && truncated == 0) return;

    char line[96];
    Writer w{line, sizeof(line), 0, false};
    append_timestamp(w);
    append_str(w, " [W] cpu");
    append_u64(w, cpu, 10, false);
    append_str(w, " kernel/log: dropped=");
    append_u64(w, dropped, 10, false);
    append_str(w, " truncated=");
    append_u64(w, truncated, 10, false);
    append_char(w, '\n');
    const usize len = finish(w);

    if (!push_bytes(g_rings[cpu], line, len)) {
        __atomic_add_fetch(&g_dropped[cpu], dropped, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_truncated[cpu], truncated, __ATOMIC_RELAXED);
    }
}

void emit_line(const char* line, usize len, bool force_uart) {
    const u32 cpu = cpu_index();
    if (__atomic_exchange_n(&g_in_log[cpu], 1u, __ATOMIC_ACQUIRE) != 0) {
        __atomic_add_fetch(&g_dropped[cpu], 1ull, __ATOMIC_RELAXED);
        return;
    }

    if (force_uart || !console_attached_for(cpu)) {
        uart_write(line, len);
    }

    if (console_attached_for(cpu)) {
        emit_drop_notice(cpu);
        if (!push_bytes(g_rings[cpu], line, len)) {
            __atomic_add_fetch(&g_dropped[cpu], 1ull, __ATOMIC_RELAXED);
        }
    }

    __atomic_store_n(&g_in_log[cpu], 0u, __ATOMIC_RELEASE);
}

usize make_prefixed_line(Level level, const char* component, const char* msg,
                         char* out, usize cap, bool* truncated) {
    Writer w{out, cap, 0, false};
    append_timestamp(w);
    append_str(w, " [");
    append_char(w, level_char(level));
    append_str(w, "] cpu");
    append_u64(w, cpu_index(), 10, false);
    append_char(w, ' ');
    append_str(w, component ? component : "kernel");
    append_str(w, ": ");

    usize msg_len = 0;
    if (!msg) msg = "(null)";
    while (msg[msg_len]) ++msg_len;
    if (msg_len > kMaxMessage) {
        msg_len = kMaxMessage;
        w.truncated = true;
    }
    for (usize i = 0; i < msg_len; ++i) append_char(w, msg[i]);
    if (msg_len == 0 || msg[msg_len - 1] != '\n') append_char(w, '\n');

    const usize len = finish(w);
    if (truncated) *truncated = w.truncated;
    return len;
}

} // namespace

void early_init() {
    g_counter_freq = read_counter_freq();
    g_boot_counter = read_counter();
    g_uart_ready = true;
}

void attach_console(Ring* const* rings, Doorbell* doorbell, u32 ring_count) {
    g_doorbell = doorbell;
    g_ring_count = ring_count > kMaxCpus ? kMaxCpus : ring_count;
    for (u32 i = 0; i < g_ring_count; ++i) {
        g_rings[i] = rings ? rings[i] : nullptr;
    }
}

void write(Level level, const char* component, const char* msg) {
    char line[kMaxLine];
    bool truncated = false;
    const usize len = make_prefixed_line(level, component, msg, line, sizeof(line), &truncated);
    if (truncated) {
        __atomic_add_fetch(&g_truncated[cpu_index()], 1ull, __ATOMIC_RELAXED);
    }
    emit_line(line, len, level == Level::Fatal);
}

void vwritef(Level level, const char* component, const char* fmt, va_list ap) {
    char msg[kMaxMessage + 1];
    bool truncated = false;
    (void)vformat(msg, sizeof(msg), fmt, ap, &truncated);
    if (truncated) {
        __atomic_add_fetch(&g_truncated[cpu_index()], 1ull, __ATOMIC_RELAXED);
    }
    write(level, component, msg);
}

void writef(Level level, const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwritef(level, component, fmt, ap);
    va_end(ap);
}

void raw(const char* msg) {
    if (!msg) msg = "(null)\n";
    write(Level::Info, "kernel", msg);
}

void vrawf(const char* fmt, va_list ap) {
    char line[kMaxMessage + 1];
    bool truncated = false;
    const usize len = vformat(line, sizeof(line), fmt, ap, &truncated);
    (void)len;
    if (truncated) {
        __atomic_add_fetch(&g_truncated[cpu_index()], 1ull, __ATOMIC_RELAXED);
    }
    write(Level::Info, "kernel", line);
}

void rawf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vrawf(fmt, ap);
    va_end(ap);
}

u64 dropped_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) {
        total += __atomic_load_n(&g_dropped[i], __ATOMIC_RELAXED);
    }
    return total;
}

u64 truncated_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) {
        total += __atomic_load_n(&g_truncated[i], __ATOMIC_RELAXED);
    }
    return total;
}

} // namespace anko::log
