// userland/msh/main.cc — Midix native shell for Mochi/libmochi.
//
// msh is intentionally not GNU bash.  It uses libmochi's BootInfo, terminal,
// ring, doorbell, FS, and cap APIs directly and exposes Midix objects as first
// class shell commands.  The parser is Bash-like enough for interactive use:
// words, quotes, backslashes, ;, &&, ||, variable assignment/substitution, and
// a compact builtin registry.

#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/terminal.h"
#include "mochi/fs.h"
#include "mochi/svc_registry.h"
#include "mochi/svc_control.h"
#include "mochi/syscall.h"
#include "dango/vm.h"
#include "midix/abi/boot.h"
#include "midix/abi/cap.h"
#include "midix/abi/protocols/fs.h"
#include "midix/abi/protocols/terminal.h"

constexpr mx_vaddr_t kVaBootInfo      = 0x1000'0000ull;
constexpr mx_vaddr_t kVaLogRing       = 0x1800'0000ull;
constexpr mx_vaddr_t kVaTermInput     = 0x2000'0000ull; // tty -> shell
constexpr mx_vaddr_t kVaTermOutput    = 0x2002'0000ull; // shell -> tty
constexpr mx_vaddr_t kVaFsCmdRing     = 0x2004'0000ull;
constexpr mx_vaddr_t kVaFsReplyRing   = 0x2006'0000ull;
constexpr mx_vaddr_t kVaSvcRegistry   = 0x2010'0000ull;
constexpr uint32_t kDefaultOrder      = MX_DEFAULT_RING_ORDER;
constexpr uint32_t kLineMax           = 256;
constexpr uint32_t kTokenMax          = 32;
constexpr uint32_t kArgMax            = 16;
constexpr uint32_t kVarMax            = 32;
constexpr uint32_t kVarNameMax        = 24;
constexpr uint32_t kVarValueMax       = 128;
constexpr uint32_t kHistoryMax        = 16;
constexpr uint32_t kPromptMax         = 96;
constexpr uint32_t kCompletionMax     = 24;

struct Token {
    char text[96];
    bool control;
};

struct ParsedLine {
    Token tokens[kTokenMax];
    uint32_t count;
};

struct EnvVar {
    char name[kVarNameMax];
    char value[kVarValueMax];
    bool used;
};

struct Shell {
    const MxBootInfo* boot;
    mochi::terminal::Terminal term;
    mochi::fs::Client fs;
    mochi::svcctl::Client svcctl;
    const mochi::svc::RegistryPage* svc_registry;
    EnvVar vars[kVarMax];
    char cwd[128];
    fs_handle_t cwd_handle;
    int32_t last_status;
    bool running;
    bool have_terminal;
    bool have_fs;
    bool have_svcctl;
};

static Shell g{};

// Midix currently gives each initial userland thread a single 4 KiB stack.
// Keep the interactive line buffer and parsed token block out of the stack;
// ParsedLine alone is ~3 KiB.  Leaving it as a local in _start() underflowed
// the initial stack and faulted just below USER_STACK_TOP - PAGE_SIZE.
static char g_line[kLineMax];
static ParsedLine g_parsed{};
static char g_history[kHistoryMax][kLineMax];
static uint32_t g_history_count = 0;
static uint32_t g_history_head = 0;

static uint32_t slen(const char* s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
}

static bool svc_registry_valid_snapshot(const mochi::svc::RegistryPage* page) {
    if (!page) return false;
    for (uint32_t tries = 0; tries < 8; ++tries) {
        const mx_u64_t gen = mochi::svc::read_begin(page);
        const bool ok = page->magic == mochi::svc::kRegistryMagic &&
                        page->version == mochi::svc::kRegistryVersion &&
                        page->entry_count <= mochi::svc::kMaxRegistryEntries;
        if (!mochi::svc::read_retry(page, gen)) return ok;
    }
    return false;
}

static uint16_t svc_registry_entry_count() {
    if (!g.svc_registry) return 0;
    for (uint32_t tries = 0; tries < 8; ++tries) {
        const mx_u64_t gen = mochi::svc::read_begin(g.svc_registry);
        const uint16_t count = g.svc_registry->entry_count;
        if (!mochi::svc::read_retry(g.svc_registry, gen)) {
            return count <= mochi::svc::kMaxRegistryEntries ? count : 0;
        }
    }
    return 0;
}

static bool svc_registry_copy_entry(uint16_t index, mochi::svc::RegistryEntry* out) {
    if (!g.svc_registry || !out) return false;
    for (uint32_t tries = 0; tries < 8; ++tries) {
        const mx_u64_t gen = mochi::svc::read_begin(g.svc_registry);
        const uint16_t count = g.svc_registry->entry_count;
        if (count > mochi::svc::kMaxRegistryEntries || index >= count) {
            if (!mochi::svc::read_retry(g.svc_registry, gen)) return false;
            continue;
        }
        *out = g.svc_registry->entries[index];
        if (!mochi::svc::read_retry(g.svc_registry, gen)) return true;
    }
    return false;
}

static bool svc_exists(const char* name) {
    mochi::svc::RegistryEntry e{};
    const uint16_t count = svc_registry_entry_count();
    for (uint16_t i = 0; i < count; ++i) {
        if (svc_registry_copy_entry(i, &e) && streq(e.name, name)) return true;
    }
    return false;
}

static void memzero(void* p, uint64_t len) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < len; ++i) b[i] = 0;
}

static void copy_str(char* dst, uint32_t cap, const char* src) {
    if (!dst || cap == 0) return;
    uint32_t i = 0;
    if (src) {
        while (i + 1 < cap && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static void append_char(char* dst, uint32_t cap, uint32_t* n, char c) {
    if (!dst || !n || cap == 0) return;
    if (*n + 1 < cap) {
        dst[*n] = c;
        ++*n;
        dst[*n] = '\0';
    }
}

static void append_str(char* dst, uint32_t cap, uint32_t* n, const char* s) {
    if (!s) s = "";
    while (*s) append_char(dst, cap, n, *s++);
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_printable(char c) {
    return c >= 32 && c <= 126;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

static uint64_t parse_u64(const char* s, bool* ok = nullptr) {
    uint64_t v = 0;
    bool any = false;
    if (!s) {
        if (ok) *ok = false;
        return 0;
    }
    uint32_t i = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        while (s[i]) {
            char c = s[i];
            uint64_t d = 0;
            if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = 10ull + static_cast<uint64_t>(c - 'a');
            else if (c >= 'A' && c <= 'F') d = 10ull + static_cast<uint64_t>(c - 'A');
            else break;
            v = (v << 4) | d;
            any = true;
            ++i;
        }
    } else {
        while (s[i] && is_digit(s[i])) {
            v = v * 10ull + static_cast<uint64_t>(s[i] - '0');
            any = true;
            ++i;
        }
    }
    if (ok) *ok = any && s[i] == '\0';
    return v;
}

static void u64_to_dec(uint64_t v, char* out, uint32_t cap) {
    if (!out || cap == 0) return;
    char tmp[24];
    uint32_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    uint32_t j = 0;
    while (n && j + 1 < cap) out[j++] = tmp[--n];
    out[j] = '\0';
}

static void u64_to_hex(uint64_t v, char* out, uint32_t cap) {
    if (!out || cap == 0) return;
    const char* hex = "0123456789abcdef";
    uint32_t n = 0;
    if (n + 1 < cap) out[n++] = '0';
    if (n + 1 < cap) out[n++] = 'x';
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint32_t d = static_cast<uint32_t>((v >> shift) & 0xfull);
        if (d || started || shift == 0) {
            started = true;
            if (n + 1 < cap) out[n++] = hex[d];
        }
    }
    out[n] = '\0';
}

static void put_bytes(const char* data, uint32_t len) {
    if (!data || len == 0) return;
    if (g.have_terminal && g.term.valid()) {
        (void)g.term.write_bytes_spinning(data, len, 0xFFFFu);
    } else {
        sys_debug_puts(data, len);
    }
}

static void put(const char* s) {
    put_bytes(s, slen(s));
}

static void putln(const char* s) {
    put(s);
    put("\n");
}

static void put_dec(uint64_t v) {
    char b[32];
    u64_to_dec(v, b, sizeof(b));
    put(b);
}

static void put_hex(uint64_t v) {
    char b[32];
    u64_to_hex(v, b, sizeof(b));
    put(b);
}

static void put_status(mx_status_t s) {
    if (s == MX_OK) { put("OK"); return; }
    put("err(");
    if (s < 0) {
        put("-");
        put_dec(static_cast<uint64_t>(-s));
    } else {
        put_dec(static_cast<uint64_t>(s));
    }
    put(")");
}

static const char* cap_type_name(uint32_t type) {
    switch (type) {
    case MX_CAP_NULL: return "null";
    case MX_CAP_THREAD: return "thread";
    case MX_CAP_PROCESS: return "process";
    case MX_CAP_PAGE: return "page";
    case MX_CAP_SHARED_RING: return "ring";
    case MX_CAP_DOORBELL: return "doorbell";
    case MX_CAP_ARENA: return "arena";
    case MX_CAP_NAMESERVICE: return "nameservice";
    case MX_CAP_SUPERVISOR: return "supervisor";
    case MX_CAP_UART: return "uart";
    case MX_CAP_TIMER: return "timer";
    case MX_CAP_VM_ROOT: return "vm_root";
    case MX_CAP_VM: return "vm";
    case MX_CAP_VCPU: return "vcpu";
    case MX_CAP_VM_MEMORY: return "vm_memory";
    case MX_CAP_VM_CHANNEL: return "vm_channel";
    case MX_CAP_SCHED_CONTEXT: return "sched_context";
    case MX_CAP_MMIO: return "mmio";
    case MX_CAP_IRQ: return "irq";
    default: return "unknown";
    }
}

static const char* boot_type_name(uint32_t type) {
    switch (type) {
    case MX_BOOT_ENTRY_NONE: return "none";
    case MX_BOOT_ENTRY_HANDLE: return "handle";
    case MX_BOOT_ENTRY_RING: return "ring";
    case MX_BOOT_ENTRY_DOORBELL: return "doorbell";
    case MX_BOOT_ENTRY_MMIO: return "mmio";
    case MX_BOOT_ENTRY_ARENA: return "arena";
    case MX_BOOT_ENTRY_ENDPOINT: return "endpoint";
    case MX_BOOT_ENTRY_VALUE: return "value";
    default: return "?";
    }
}

static EnvVar* find_var(const char* name) {
    for (uint32_t i = 0; i < kVarMax; ++i) {
        if (g.vars[i].used && streq(g.vars[i].name, name)) return &g.vars[i];
    }
    return nullptr;
}

static const char* get_var(const char* name) {
    if (streq(name, "?")) {
        static char last[16];
        u64_to_dec(static_cast<uint64_t>(g.last_status < 0 ? -g.last_status : g.last_status), last, sizeof(last));
        return last;
    }
    if (streq(name, "PWD")) return g.cwd;
    EnvVar* v = find_var(name);
    return v ? v->value : "";
}

static bool set_var(const char* name, const char* value) {
    if (!name || !name[0]) return false;
    EnvVar* v = find_var(name);
    if (!v) {
        for (uint32_t i = 0; i < kVarMax; ++i) {
            if (!g.vars[i].used) {
                v = &g.vars[i];
                v->used = true;
                copy_str(v->name, sizeof(v->name), name);
                break;
            }
        }
    }
    if (!v) return false;
    copy_str(v->value, sizeof(v->value), value ? value : "");
    return true;
}

static bool token_is_assignment(const char* s) {
    if (!s || !s[0]) return false;
    uint32_t i = 0;
    if (!(s[i] == '_' || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) return false;
    ++i;
    while (s[i] && s[i] != '=') {
        if (!(s[i] == '_' || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return false;
        ++i;
    }
    return s[i] == '=';
}

static uint32_t build_prompt(char* out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    uint32_t n = 0;
    append_str(out, cap, &n, "msh:");
    append_str(out, cap, &n, g.cwd);
    append_str(out, cap, &n, "$ ");
    return n;
}

static void history_push(const char* line) {
    if (!line || !line[0]) return;
    if (g_history_count > 0) {
        const uint32_t last = (g_history_head + kHistoryMax - 1u) % kHistoryMax;
        if (streq(g_history[last], line)) return;
    }
    copy_str(g_history[g_history_head], kLineMax, line);
    g_history_head = (g_history_head + 1u) % kHistoryMax;
    if (g_history_count < kHistoryMax) ++g_history_count;
}

static const char* history_get(uint32_t index_from_oldest) {
    if (index_from_oldest >= g_history_count) return nullptr;
    const uint32_t start = (g_history_head + kHistoryMax - g_history_count) % kHistoryMax;
    const uint32_t idx = (start + index_from_oldest) % kHistoryMax;
    return g_history[idx];
}

static void render_edit_line(const char* prompt, const char* buffer,
                             uint32_t len, uint32_t cursor,
                             uint32_t* prev_render_len) {
    put("\r");
    put(prompt);
    if (len) put_bytes(buffer, len);
    uint32_t prev = prev_render_len ? *prev_render_len : 0;
    while (prev > len) {
        put(" ");
        --prev;
    }
    put("\r");
    put(prompt);
    if (cursor) put_bytes(buffer, cursor);
    if (prev_render_len) *prev_render_len = len;
}

enum CompletionKind {
    CompleteFirstToken,
    CompleteSvcVerb,
    CompleteSvcName,
    CompleteCapVerb,
    CompleteArenaVerb,
    CompleteRingVerb,
    CompleteDoorbellVerb,
    CompleteVmVerb,
};

static bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    uint32_t i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return false;
        ++i;
    }
    return true;
}

static bool add_match(const char* candidate, const char* prefix,
                      const char** matches, uint32_t* count, uint32_t max_matches) {
    if (!candidate || !matches || !count) return false;
    if (!starts_with(candidate, prefix ? prefix : "")) return false;
    for (uint32_t i = 0; i < *count; ++i) {
        if (streq(matches[i], candidate)) return false;
    }
    if (*count < max_matches) {
        matches[*count] = candidate;
        ++*count;
    }
    return true;
}

static void print_completion_matches(const char** matches, uint32_t count) {
    if (count == 0) return;
    put("\n");
    for (uint32_t i = 0; i < count; ++i) {
        if (i) put("  ");
        put(matches[i]);
    }
    put("\n");
}

static uint32_t longest_common_prefix(const char** matches, uint32_t count) {
    if (count == 0 || !matches[0]) return 0;
    uint32_t n = slen(matches[0]);
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t j = 0;
        while (j < n && matches[i][j] && matches[0][j] == matches[i][j]) ++j;
        n = j;
    }
    return n;
}

static CompletionKind completion_kind_for_tokens(uint32_t token_count, char tokens[][96]) {
    if (token_count == 0) return CompleteFirstToken;
    if (token_count == 1) {
        if (streq(tokens[0], "svc")) return CompleteSvcVerb;
        if (streq(tokens[0], "cap")) return CompleteCapVerb;
        if (streq(tokens[0], "arena")) return CompleteArenaVerb;
        if (streq(tokens[0], "ring")) return CompleteRingVerb;
        if (streq(tokens[0], "doorbell")) return CompleteDoorbellVerb;
        if (streq(tokens[0], "vm")) return CompleteVmVerb;
        return CompleteFirstToken;
    }
    if (streq(tokens[0], "svc") && token_count == 2 &&
        (streq(tokens[1], "deps") || streq(tokens[1], "log") ||
         streq(tokens[1], "start") || streq(tokens[1], "stop") || streq(tokens[1], "restart"))) {
        return CompleteSvcName;
    }
    return CompleteFirstToken;
}

static bool split_for_completion(const char* line, uint32_t len,
                                 char tokens[][96], uint32_t* token_count,
                                 char* prefix_out, uint32_t prefix_cap,
                                 bool* at_token_start) {
    if (!token_count || !prefix_out || prefix_cap == 0 || !at_token_start) return false;
    *token_count = 0;
    *at_token_start = (len == 0);
    prefix_out[0] = '\0';
    uint32_t i = 0;
    while (i < len) {
        while (i < len && is_space(line[i])) ++i;
        if (i >= len) { *at_token_start = true; break; }
        if (*token_count >= 4) break;
        uint32_t n = 0;
        while (i < len && !is_space(line[i])) append_char(tokens[*token_count], 96, &n, line[i++]);
        ++*token_count;
        *at_token_start = (i < len && is_space(line[i]));
    }
    if (!*at_token_start && *token_count > 0) {
        copy_str(prefix_out, prefix_cap, tokens[*token_count - 1]);
        tokens[*token_count - 1][0] = '\0';
        --*token_count;
    }
    return true;
}

static bool try_complete_line(char* line, uint32_t cap, uint32_t* len_io, uint32_t* cursor_io,
                              const char* prompt, uint32_t* prev_render_len);

static bool parse_line(const char* line, ParsedLine* out) {
    if (!out) return false;
    out->count = 0;
    uint32_t i = 0;
    while (line && line[i]) {
        while (is_space(line[i])) ++i;
        if (!line[i]) break;
        if (line[i] == '#') break;
        if (out->count >= kTokenMax) return false;

        Token& t = out->tokens[out->count];
        memzero(&t, sizeof(t));
        uint32_t n = 0;

        if (line[i] == ';') {
            t.control = true;
            append_char(t.text, sizeof(t.text), &n, ';');
            ++i;
            ++out->count;
            continue;
        }
        if (line[i] == '&' && line[i + 1] == '&') {
            t.control = true;
            append_str(t.text, sizeof(t.text), &n, "&&");
            i += 2;
            ++out->count;
            continue;
        }
        if (line[i] == '|' && line[i + 1] == '|') {
            t.control = true;
            append_str(t.text, sizeof(t.text), &n, "||");
            i += 2;
            ++out->count;
            continue;
        }

        bool single = false;
        bool dbl = false;
        while (line[i]) {
            const char c = line[i];
            if (!single && !dbl && (is_space(c) || c == ';' ||
                (c == '&' && line[i + 1] == '&') ||
                (c == '|' && line[i + 1] == '|'))) {
                break;
            }
            if (!dbl && c == '\'') {
                single = !single;
                ++i;
                continue;
            }
            if (!single && c == '"') {
                dbl = !dbl;
                ++i;
                continue;
            }
            if (!single && c == '\\' && line[i + 1]) {
                append_char(t.text, sizeof(t.text), &n, line[i + 1]);
                i += 2;
                continue;
            }
            if (!single && c == '$') {
                ++i;
                char name[32];
                uint32_t nn = 0;
                if (line[i] == '?') {
                    append_char(name, sizeof(name), &nn, '?');
                    ++i;
                } else {
                    while (is_name_char(line[i]) && nn + 1 < sizeof(name)) {
                        append_char(name, sizeof(name), &nn, line[i]);
                        ++i;
                    }
                }
                append_str(t.text, sizeof(t.text), &n, get_var(name));
                continue;
            }
            append_char(t.text, sizeof(t.text), &n, c);
            ++i;
        }
        if (single || dbl) {
            putln("msh: unmatched quote");
            return false;
        }
        ++out->count;
    }
    return true;
}

static bool try_complete_line(char* line, uint32_t cap, uint32_t* len_io, uint32_t* cursor_io,
                              const char* prompt, uint32_t* prev_render_len) {
    if (!line || !len_io || !cursor_io) return false;
    char tokens[4][96]{};
    uint32_t token_count = 0;
    char prefix[96]{};
    bool at_token_start = false;
    if (!split_for_completion(line, *cursor_io, tokens, &token_count, prefix, sizeof(prefix), &at_token_start)) {
        return false;
    }
    CompletionKind kind = completion_kind_for_tokens(token_count, tokens);
    const char* matches[kCompletionMax]{};
    uint32_t match_count = 0;
    auto add_literal = [&](const char* value) {
        (void)add_match(value, prefix, matches, &match_count, kCompletionMax);
    };

    if (kind == CompleteFirstToken) {
        add_literal("help"); add_literal("exit"); add_literal("echo"); add_literal("printf");
        add_literal("clear"); add_literal("set"); add_literal("unset"); add_literal("pwd");
        add_literal("cd"); add_literal("ls"); add_literal("cat"); add_literal("bootinfo");
        add_literal("svc"); add_literal("cap"); add_literal("arena"); add_literal("ring");
        add_literal("doorbell"); add_literal("vm"); add_literal("shutdown"); add_literal("reboot");
        add_literal("mkdir"); add_literal("rm"); add_literal("touch");
    } else if (kind == CompleteSvcVerb) {
        add_literal("list"); add_literal("tree"); add_literal("groups"); add_literal("deps");
        add_literal("log"); add_literal("start"); add_literal("stop"); add_literal("restart");
    } else if (kind == CompleteCapVerb) {
        add_literal("ls"); add_literal("tree"); add_literal("inspect"); add_literal("revoke");
    } else if (kind == CompleteArenaVerb) {
        add_literal("ls"); add_literal("show");
    } else if (kind == CompleteRingVerb) {
        add_literal("stats");
    } else if (kind == CompleteDoorbellVerb) {
        add_literal("stats");
    } else if (kind == CompleteVmVerb) {
        add_literal("list"); add_literal("start"); add_literal("console");
    } else if (kind == CompleteSvcName && g.svc_registry) {
        mochi::svc::RegistryEntry e{};
        const uint16_t count = svc_registry_entry_count();
        for (uint16_t i = 0; i < count; ++i) {
            if (svc_registry_copy_entry(i, &e)) add_literal(e.name);
        }
    }
    if (match_count == 0) return false;

    const uint32_t prefix_len = slen(prefix);
    const uint32_t lcp = longest_common_prefix(matches, match_count);
    if (match_count == 1 || lcp > prefix_len) {
        const char* chosen = matches[0];
        const uint32_t target_len = (match_count == 1) ? slen(chosen) : lcp;
        const uint32_t suffix_len = target_len > prefix_len ? (target_len - prefix_len) : 0;
        const uint32_t extra_space = (match_count == 1) ? 1u : 0u;
        if (*len_io + suffix_len + extra_space >= cap) return false;

        for (uint32_t i = *len_io + extra_space + suffix_len; i > *cursor_io + suffix_len + extra_space; --i) {
            line[i - 1] = line[i - 1 - suffix_len - extra_space];
        }
        for (uint32_t i = 0; i < suffix_len; ++i) line[*cursor_io + i] = chosen[prefix_len + i];
        *cursor_io += suffix_len;
        *len_io += suffix_len;
        if (extra_space) {
            line[*cursor_io] = ' ';
            ++*cursor_io;
            ++*len_io;
        }
        line[*len_io] = '\0';
        render_edit_line(prompt, line, *len_io, *cursor_io, prev_render_len);
        return true;
    }

    print_completion_matches(matches, match_count);
    render_edit_line(prompt, line, *len_io, *cursor_io, prev_render_len);
    return true;
}

static bool read_line(char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uint32_t len = 0;
    uint32_t cursor = 0;
    uint32_t prev_render_len = 0;
    char prompt_text[kPromptMax];
    build_prompt(prompt_text, sizeof(prompt_text));
    int32_t history_nav = -1;
    char live_line[kLineMax]{};

    auto load_history = [&](int32_t nav) {
        if (g_history_count == 0) return;
        if (nav < 0) nav = 0;
        if (nav >= static_cast<int32_t>(g_history_count)) nav = static_cast<int32_t>(g_history_count) - 1;
        const uint32_t idx = g_history_count - 1u - static_cast<uint32_t>(nav);
        const char* entry = history_get(idx);
        if (!entry) return;
        copy_str(out, cap, entry);
        len = slen(out);
        cursor = len;
        history_nav = nav;
        render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
    };

    RingSlot slot{};
    int esc_state = 0;
    char esc_param = '\0';
    for (;;) {
        if (!g.have_terminal || !g.term.valid()) return false;
        if (!g.term.wait_read(&slot)) return false;
        if (slot.type != MX_TERM_INPUT_BYTES) continue;
        for (uint32_t i = 0; i < slot.length; ++i) {
            char c = static_cast<char>(slot.payload[i]);
            if (esc_state == 1) {
                if (c == '[') { esc_state = 2; continue; }
                esc_state = 0;
            } else if (esc_state == 2) {
                if (c == 'A') {
                    if (history_nav < 0) copy_str(live_line, sizeof(live_line), out);
                    load_history(history_nav < 0 ? 0 : history_nav + 1);
                    esc_state = 0;
                    continue;
                }
                if (c == 'B') {
                    if (g_history_count == 0) { esc_state = 0; continue; }
                    if (history_nav > 0) load_history(history_nav - 1);
                    else if (history_nav == 0) {
                        copy_str(out, cap, live_line);
                        len = slen(out);
                        cursor = len;
                        history_nav = -1;
                        render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                    }
                    esc_state = 0;
                    continue;
                }
                if (c == 'C') { if (cursor < len) { ++cursor; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); } esc_state = 0; continue; }
                if (c == 'D') { if (cursor > 0) { --cursor; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); } esc_state = 0; continue; }
                if (c == 'H') { cursor = 0; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); esc_state = 0; continue; }
                if (c == 'F') { cursor = len; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); esc_state = 0; continue; }
                if (c >= '0' && c <= '9') { esc_param = c; esc_state = 3; continue; }
                esc_state = 0;
            } else if (esc_state == 3) {
                if (esc_param == '3' && c == '~') {
                    if (cursor < len) {
                        for (uint32_t j = cursor; j < len; ++j) out[j] = out[j + 1];
                        --len;
                        render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                    }
                }
                esc_state = 0;
                continue;
            }
            if (c == 27) { esc_state = 1; continue; }
            if (c == '\r') c = '\n';
            if (c == 16) { if (history_nav < 0) copy_str(live_line, sizeof(live_line), out); load_history(history_nav < 0 ? 0 : history_nav + 1); continue; }
            if (c == 14) {
                if (history_nav > 0) load_history(history_nav - 1);
                else if (history_nav == 0) {
                    copy_str(out, cap, live_line);
                    len = slen(out);
                    cursor = len;
                    history_nav = -1;
                    render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                }
                continue;
            }
            if (c == 4) {
                if (len == 0) return false;
                if (cursor < len) {
                    for (uint32_t j = cursor; j < len; ++j) out[j] = out[j + 1];
                    --len;
                    render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                }
                continue;
            }
            if (c == 3) { put("^C\n"); out[0] = '\0'; return true; }
            if (c == 1) { cursor = 0; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); continue; }
            if (c == 5) { cursor = len; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); continue; }
            if (c == 2) { if (cursor > 0) { --cursor; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); } continue; }
            if (c == 6) { if (cursor < len) { ++cursor; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); } continue; }
            if (c == 21) {
                for (uint32_t j = cursor; j <= len; ++j) out[j - cursor] = out[j];
                len -= cursor;
                cursor = 0;
                render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                continue;
            }
            if (c == 11) { out[cursor] = '\0'; len = cursor; render_edit_line(prompt_text, out, len, cursor, &prev_render_len); continue; }
            if (c == 12) { for (uint32_t j = 0; j < 48; ++j) put("\n"); render_edit_line(prompt_text, out, len, cursor, &prev_render_len); continue; }
            if (c == '\b' || c == 0x7f) {
                if (cursor > 0) {
                    for (uint32_t j = cursor - 1; j < len; ++j) out[j] = out[j + 1];
                    --cursor;
                    --len;
                    render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                }
                continue;
            }
            if (c == '\t') { (void)try_complete_line(out, cap, &len, &cursor, prompt_text, &prev_render_len); continue; }
            if (c == '\n') { put("\n"); out[len] = '\0'; history_push(out); return true; }
            if (is_printable(c) || c == '\t') {
                if (len + 1 < cap) {
                    for (uint32_t j = len + 1; j > cursor; --j) out[j] = out[j - 1];
                    out[cursor] = c;
                    ++cursor;
                    ++len;
                    out[len] = '\0';
                    history_nav = -1;
                    render_edit_line(prompt_text, out, len, cursor, &prev_render_len);
                }
            }
        }
    }
}

static bool map_ring(const MxBootInfo* boot, const char* name, mx_vaddr_t va,
                     RingPage** out_ring, uint32_t* out_order) {
    const MxBootEntry* e = mochi::boot::find_type(boot, name, MX_BOOT_ENTRY_RING);
    if (!e || e->handle == MX_NULL_HANDLE) return false;
    if (mx_map_page(e->handle, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) return false;
    if (out_ring) *out_ring = reinterpret_cast<RingPage*>(va);
    if (out_order) *out_order = e->aux0 ? static_cast<uint32_t>(e->aux0) : kDefaultOrder;
    return true;
}

static bool boot_map_term(const MxBootInfo* boot) {
    RingPage* input = nullptr;
    RingPage* output = nullptr;
    uint32_t input_order = kDefaultOrder;
    uint32_t output_order = kDefaultOrder;
    if (!map_ring(boot, "term.input.ring", kVaTermInput, &input, &input_order)) return false;
    if (!map_ring(boot, "term.output.ring", kVaTermOutput, &output, &output_order)) return false;
    if (input_order != output_order) {
        // libmochi::SpscEndpoint has one order field.  Init should create both
        // terminal rings with the same order.
        return false;
    }
    g.term.endpoint.rx_ring = input;
    g.term.endpoint.tx_ring = output;
    g.term.endpoint.order = input_order;
    g.term.endpoint.rx_doorbell = mochi::boot::handle(boot, "term.input.doorbell");
    g.term.endpoint.tx_doorbell = mochi::boot::handle(boot, "term.output.doorbell");
    g.have_terminal = g.term.valid();
    return g.have_terminal;
}

static bool boot_map_fs(const MxBootInfo* boot) {
    RingPage* cmd = nullptr;
    RingPage* reply = nullptr;
    uint32_t cmd_order = kDefaultOrder;
    uint32_t reply_order = kDefaultOrder;
    if (!map_ring(boot, "fs.cmd.ring", kVaFsCmdRing, &cmd, &cmd_order)) return false;
    if (!map_ring(boot, "fs.reply.ring", kVaFsReplyRing, &reply, &reply_order)) return false;
    if (cmd_order != reply_order) return false;
    g.fs.ep.tx_ring = cmd;
    g.fs.ep.rx_ring = reply;
    g.fs.ep.order = cmd_order;
    g.fs.ep.tx_doorbell = mochi::boot::handle(boot, "fs.cmd.doorbell");
    g.fs.ep.rx_doorbell = mochi::boot::handle(boot, "fs.reply.doorbell");
    g.have_fs = g.fs.valid();
    return g.have_fs;
}

static bool boot_map_svc_registry(const MxBootInfo* boot) {
    const mx_handle_t h = mochi::boot::handle(boot, "svc.registry");
    if (h == MX_NULL_HANDLE) return false;
    if (mx_map_page(h, kVaSvcRegistry, MX_RIGHT_READ) != MX_OK) return false;
    const auto* page = reinterpret_cast<const mochi::svc::RegistryPage*>(kVaSvcRegistry);
    if (!svc_registry_valid_snapshot(page)) return false;
    g.svc_registry = page;
    return true;
}

static bool boot_map_svcctl(const MxBootInfo* boot) {
    RingPage* cmd = nullptr;
    RingPage* reply = nullptr;
    uint32_t cmd_order = kDefaultOrder;
    uint32_t reply_order = kDefaultOrder;
    if (!map_ring(boot, "svc.cmd.ring", kVaFsReplyRing + 0x10000, &cmd, &cmd_order)) return false;
    if (!map_ring(boot, "svc.reply.ring", kVaFsReplyRing + 0x20000, &reply, &reply_order)) return false;
    if (cmd_order != reply_order) return false;
    g.svcctl.ep.tx_ring = cmd;
    g.svcctl.ep.rx_ring = reply;
    g.svcctl.ep.order = cmd_order;
    g.svcctl.ep.tx_doorbell = mochi::boot::handle(boot, "svc.cmd.doorbell");
    g.svcctl.ep.rx_doorbell = mochi::boot::handle(boot, "svc.reply.doorbell");
    g.have_svcctl = g.svcctl.valid();
    return g.have_svcctl;
}

static int cmd_help(int argc, char** argv);
static int cmd_exit(int argc, char** argv);
static int cmd_echo(int argc, char** argv);
static int cmd_printf(int argc, char** argv);
static int cmd_clear(int argc, char** argv);
static int cmd_set(int argc, char** argv);
static int cmd_unset(int argc, char** argv);
static int notimpl(const char* what);

static int cmd_pwd(int, char**) {
    putln(g.cwd);
    return 0;
}

static bool fs_open_name_at(fs_handle_t parent, const char* name, mx_u32_t flags, FsOpenReply* out) {
    if (!g.have_fs) return false;
    return g.fs.wait_open(parent, name, flags, out);
}

static bool path_push(char parts[][36], uint32_t* count, const char* s, uint32_t len) {
    if (!parts || !count || len == 0) return true;
    if (len == 1 && s[0] == '.') return true;
    if (len == 2 && s[0] == '.' && s[1] == '.') {
        if (*count > 0) --*count;
        return true;
    }
    if (*count >= 12 || len >= 36) return false;
    for (uint32_t i = 0; i < len; ++i) parts[*count][i] = s[i];
    parts[*count][len] = '\0';
    ++*count;
    return true;
}

static bool path_collect(const char* path, char parts[][36], uint32_t* count) {
    if (!path || !parts || !count) return false;
    uint32_t i = 0;
    while (path[i]) {
        while (path[i] == '/') ++i;
        const uint32_t start = i;
        while (path[i] && path[i] != '/') ++i;
        if (i > start && !path_push(parts, count, path + start, i - start)) return false;
    }
    return true;
}

static bool normalize_path(const char* input, char* out, uint32_t out_cap) {
    if (!input || !input[0]) input = ".";
    if (!out || out_cap == 0) return false;
    char parts[12][36]{};
    uint32_t count = 0;
    if (input[0] != '/') {
        if (!path_collect(g.cwd, parts, &count)) return false;
    }
    if (!path_collect(input, parts, &count)) return false;
    uint32_t n = 0;
    append_char(out, out_cap, &n, '/');
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) append_char(out, out_cap, &n, '/');
        append_str(out, out_cap, &n, parts[i]);
    }
    return n < out_cap;
}

static bool split_parent_name(const char* path, char* parent, uint32_t parent_cap,
                              char* name, uint32_t name_cap) {
    char norm[128];
    if (!normalize_path(path, norm, sizeof(norm))) return false;
    if (streq(norm, "/")) return false;
    uint32_t last = 0;
    for (uint32_t i = 0; norm[i]; ++i) if (norm[i] == '/') last = i;
    if (last == 0) {
        copy_str(parent, parent_cap, "/");
        copy_str(name, name_cap, norm + 1);
    } else {
        uint32_t n = 0;
        for (uint32_t i = 0; i < last && n + 1 < parent_cap; ++i) parent[n++] = norm[i];
        parent[n] = '\0';
        copy_str(name, name_cap, norm + last + 1);
    }
    return name[0] != '\0';
}

struct ResolvedPath {
    fs_handle_t handle;
    mx_u64_t inode;
    mx_u16_t type;
    mx_u64_t size;
    char normalized[128];
    bool borrowed_root;
};

static void close_resolved(const ResolvedPath& r) {
    if (!r.borrowed_root && r.handle != 0 && r.handle != g.cwd_handle) {
        (void)g.fs.try_close(r.handle);
    }
}

static bool resolve_normalized(const char* normalized, mx_u32_t flags, ResolvedPath* out) {
    if (!normalized || !out || !g.have_fs) return false;
    *out = {};
    copy_str(out->normalized, sizeof(out->normalized), normalized);
    if (streq(normalized, "/")) {
        out->handle = 1;
        out->inode = 1;
        out->type = FS_INODE_DIR;
        out->size = 0;
        out->borrowed_root = true;
        return true;
    }

    fs_handle_t parent = 1;
    bool parent_owned = false;
    uint32_t i = 1;
    while (normalized[i]) {
        char component[36]{};
        uint32_t n = 0;
        while (normalized[i] && normalized[i] != '/') {
            if (n + 1 >= sizeof(component)) {
                if (parent_owned) (void)g.fs.try_close(parent);
                return false;
            }
            component[n++] = normalized[i++];
        }
        component[n] = '\0';
        while (normalized[i] == '/') ++i;
        const bool final = normalized[i] == '\0';
        FsOpenReply open{};
        const mx_u32_t open_flags = final ? flags : FS_OPEN_READ;
        if (!fs_open_name_at(parent, component, open_flags, &open) || open.status != MX_OK) {
            if (parent_owned) (void)g.fs.try_close(parent);
            return false;
        }
        if (parent_owned) (void)g.fs.try_close(parent);
        if (!final && open.type != FS_INODE_DIR) {
            (void)g.fs.try_close(open.handle);
            return false;
        }
        if (final) {
            out->handle = open.handle;
            out->inode = open.inode_number;
            out->type = open.type;
            out->size = open.size;
            out->borrowed_root = false;
            return true;
        }
        parent = open.handle;
        parent_owned = true;
    }
    return false;
}

static bool resolve_path(const char* path, mx_u32_t flags, ResolvedPath* out) {
    char norm[128];
    if (!normalize_path(path, norm, sizeof(norm))) return false;
    return resolve_normalized(norm, flags, out);
}

static bool wait_status_reply(mx_u32_t type, mx_i32_t* out) {
    RingSlot slot{};
    while (g.fs.ep.wait_recv(&slot)) {
        if (slot.type == type && slot.length >= sizeof(mx_i32_t)) {
            mx_i32_t status = MX_ERR_BAD_PROTOCOL;
            mochi::fs::copy_bytes(&status, slot.payload, sizeof(status));
            if (out) *out = status;
            return true;
        }
        if (slot.type == FS_ERROR && slot.length >= sizeof(mx_i32_t)) {
            mx_i32_t status = MX_ERR_BAD_PROTOCOL;
            mochi::fs::copy_bytes(&status, slot.payload, sizeof(status));
            if (out) *out = status;
            return true;
        }
    }
    return false;
}

static int cmd_cd(int argc, char** argv) {
    if (!g.have_fs) return notimpl("cd io.fs");
    const char* dst = argc > 1 ? argv[1] : "/";
    ResolvedPath r{};
    if (!resolve_path(dst, FS_OPEN_READ, &r)) {
        put("msh: cd: no such directory: "); putln(dst);
        return 1;
    }
    if (r.type != FS_INODE_DIR) {
        put("msh: cd: not a directory: "); putln(dst);
        close_resolved(r);
        return 1;
    }
    if (g.cwd_handle != 1 && g.cwd_handle != r.handle) (void)g.fs.try_close(g.cwd_handle);
    copy_str(g.cwd, sizeof(g.cwd), r.normalized);
    g.cwd_handle = r.handle;
    set_var("PWD", g.cwd);
    return 0;
}

static int cmd_ls(int argc, char** argv) {
    if (!g.have_fs) return notimpl("ls io.fs");
    const char* name = argc > 1 ? argv[1] : ".";
    ResolvedPath r{};
    if (!resolve_path(name, FS_OPEN_READ, &r)) {
        put("msh: ls: cannot access: "); putln(name);
        return 1;
    }
    if (r.type != FS_INODE_DIR) {
        const char* display = name;
        for (uint32_t i = 0; name[i]; ++i) if (name[i] == '/') display = name + i + 1;
        putln(display);
        close_resolved(r);
        return 0;
    }
    mx_u32_t cookie = 0;
    bool any = false;
    for (;;) {
        FsReaddirRequest req{};
        req.handle = r.handle;
        req.cookie = cookie;
        if (!g.fs.ep.send_spinning(FS_READDIR, &req, sizeof(req))) {
            close_resolved(r);
            return 1;
        }
        RingSlot slot{};
        FsReaddirReply reply{};
        bool got = false;
        while (g.fs.ep.wait_recv(&slot)) {
            if (slot.type == FS_READDIR_REPLY && slot.length >= sizeof(FsReaddirReply)) {
                mochi::fs::copy_bytes(&reply, slot.payload, sizeof(reply));
                got = true;
                break;
            }
            if (slot.type == FS_ERROR) break;
        }
        if (!got || reply.status != MX_OK) {
            put("msh: ls: "); put_status(reply.status); put("\n");
            close_resolved(r);
            return 1;
        }
        if (reply.flags & FS_READDIR_HAS_ENTRY) {
            if (reply.type == FS_INODE_DIR) put("d "); else put("- ");
            put_bytes(reply.name, reply.name_len);
            if (reply.flags & FS_READDIR_TRUNCATED) put("…");
            put("\n");
            any = true;
        }
        if (reply.flags & FS_READDIR_EOF) break;
        cookie = reply.next_cookie;
    }
    if (!any) putln(".");
    close_resolved(r);
    return 0;
}

static int cmd_cat(int argc, char** argv) {
    if (argc < 2) { putln("usage: cat FILE"); return 1; }
    if (!g.have_fs) return notimpl("cat io.fs");
    ResolvedPath r{};
    if (!resolve_path(argv[1], FS_OPEN_READ, &r)) {
        put("msh: cat: open failed: "); putln(argv[1]);
        return 1;
    }
    if (r.type != FS_INODE_FILE) {
        put("msh: cat: not a file: "); putln(argv[1]);
        close_resolved(r);
        return 1;
    }
    mx_u64_t off = 0;
    while (true) {
        FsReadReply read{};
        if (!g.fs.try_read(r.handle, off, FS_INLINE_READ_MAX, &read)) {
            putln("msh: cat: read protocol failed");
            close_resolved(r);
            return 1;
        }
        if (read.status != MX_OK) {
            put("msh: cat: "); put_status(read.status); put("\n");
            close_resolved(r);
            return 1;
        }
        if (read.length_read == 0) break;
        put_bytes(reinterpret_cast<const char*>(read.inline_data), read.length_read);
        off += read.length_read;
        if (read.length_read < FS_INLINE_READ_MAX) break;
    }
    close_resolved(r);
    return 0;
}

static int cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { putln("usage: mkdir DIR"); return 1; }
    if (!g.have_fs) return notimpl("mkdir io.fs");
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        char parent_path[128]{};
        char name[36]{};
        if (!split_parent_name(argv[a], parent_path, sizeof(parent_path), name, sizeof(name))) {
            put("mkdir: bad path: "); putln(argv[a]); rc = 1; continue;
        }
        ResolvedPath parent{};
        if (!resolve_path(parent_path, FS_OPEN_READ, &parent) || parent.type != FS_INODE_DIR) {
            put("mkdir: parent missing: "); putln(parent_path); rc = 1; continue;
        }
        FsMkdirRequest req{};
        req.parent_inode = parent.handle;
        req.mode = 0755;
        uint32_t n = slen(name);
        if (n > sizeof(req.name)) n = sizeof(req.name);
        req.name_len = static_cast<mx_u8_t>(n);
        for (uint32_t i = 0; i < n; ++i) req.name[i] = name[i];
        mx_i32_t status = MX_ERR_BAD_PROTOCOL;
        if (!g.fs.ep.send_spinning(FS_MKDIR, &req, sizeof(req)) || !wait_status_reply(FS_MKDIR_REPLY, &status)) {
            status = MX_ERR_BAD_PROTOCOL;
        }
        close_resolved(parent);
        if (status != MX_OK) { put("mkdir: "); put_status(status); put(" "); putln(argv[a]); rc = 1; }
    }
    return rc;
}

static int cmd_rm(int argc, char** argv) {
    if (argc < 2) { putln("usage: rm PATH"); return 1; }
    if (!g.have_fs) return notimpl("rm io.fs");
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        char parent_path[128]{};
        char name[36]{};
        if (!split_parent_name(argv[a], parent_path, sizeof(parent_path), name, sizeof(name))) {
            put("rm: bad path: "); putln(argv[a]); rc = 1; continue;
        }
        ResolvedPath parent{};
        if (!resolve_path(parent_path, FS_OPEN_READ, &parent) || parent.type != FS_INODE_DIR) {
            put("rm: parent missing: "); putln(parent_path); rc = 1; continue;
        }
        FsUnlinkRequest req{};
        req.parent_inode = parent.handle;
        uint32_t n = slen(name);
        if (n > sizeof(req.name)) n = sizeof(req.name);
        req.name_len = static_cast<mx_u8_t>(n);
        for (uint32_t i = 0; i < n; ++i) req.name[i] = name[i];
        mx_i32_t status = MX_ERR_BAD_PROTOCOL;
        if (!g.fs.ep.send_spinning(FS_UNLINK, &req, sizeof(req)) || !wait_status_reply(FS_UNLINK_REPLY, &status)) {
            status = MX_ERR_BAD_PROTOCOL;
        }
        close_resolved(parent);
        if (status != MX_OK) { put("rm: "); put_status(status); put(" "); putln(argv[a]); rc = 1; }
    }
    return rc;
}

static int cmd_touch(int argc, char** argv) {
    if (argc < 2) { putln("usage: touch FILE"); return 1; }
    if (!g.have_fs) return notimpl("touch io.fs");
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        char parent_path[128]{};
        char name[36]{};
        if (!split_parent_name(argv[a], parent_path, sizeof(parent_path), name, sizeof(name))) {
            put("touch: bad path: "); putln(argv[a]); rc = 1; continue;
        }
        ResolvedPath parent{};
        if (!resolve_path(parent_path, FS_OPEN_READ, &parent) || parent.type != FS_INODE_DIR) {
            put("touch: parent missing: "); putln(parent_path); rc = 1; continue;
        }
        FsOpenReply open{};
        if (!fs_open_name_at(parent.handle, name, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, &open) || open.status != MX_OK) {
            put("touch: "); put_status(open.status); put(" "); putln(argv[a]); rc = 1;
        } else {
            (void)g.fs.try_close(open.handle);
        }
        close_resolved(parent);
    }
    return rc;
}

static int cmd_bootinfo(int argc, char** argv);
static int cmd_svc(int argc, char** argv);
static int cmd_cap(int argc, char** argv);
static int cmd_arena(int argc, char** argv);
static int cmd_ring(int argc, char** argv);
static int cmd_doorbell(int argc, char** argv);
static int cmd_vm(int argc, char** argv);
static int cmd_shutdown(int argc, char** argv);
static int cmd_reboot(int argc, char** argv);
static int cmd_ring_stats(int argc, char** argv);
static int cmd_doorbell_stats(int argc, char** argv);

struct Builtin {
    const char* name;
    int (*fn)(int, char**);
    const char* help;
};

static const Builtin kBuiltins[] = {
    {"help", cmd_help, "show builtin commands"},
    {"exit", cmd_exit, "exit msh"},
    {"echo", cmd_echo, "print arguments"},
    {"printf", cmd_printf, "print arguments without implicit newline unless requested"},
    {"clear", cmd_clear, "clear terminal by scrolling"},
    {"set", cmd_set, "set or list shell variables"},
    {"unset", cmd_unset, "unset a shell variable"},
    {"pwd", cmd_pwd, "print current shell directory"},
    {"cd", cmd_cd, "change shell directory"},
    {"ls", cmd_ls, "list directory entries"},
    {"cat", cmd_cat, "read an io.fs file"},
    {"mkdir", cmd_mkdir, "create an io.fs directory"},
    {"rm", cmd_rm, "remove an io.fs file or empty directory"},
    {"touch", cmd_touch, "create an empty io.fs file"},
    {"bootinfo", cmd_bootinfo, "show BootInfo entries"},
    {"svc", cmd_svc, "service registry: list/tree/groups/deps/log/start/stop/restart"},
    {"cap", cmd_cap, "capability tools: ls/tree/inspect/revoke"},
    {"arena", cmd_arena, "arena tools: ls/show"},
    {"ring", cmd_ring, "ring stats"},
    {"doorbell", cmd_doorbell, "doorbell stats"},
    {"vm", cmd_vm, "VM tools"},
    {"shutdown", cmd_shutdown, "stop userland services and halt init"},
    {"reboot", cmd_reboot, "restart userland services"},
};

static int notimpl(const char* what) {
    put("msh: ");
    put(what);
    putln(": service API not present in current libmochi/Ame tree");
    return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
}

static void put_spaces(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) put(" ");
}

static void put_padded(const char* s, uint32_t width) {
    const uint32_t n = slen(s);
    put(s);
    if (n < width) put_spaces(width - n);
}

static void put_rule(uint32_t width) {
    for (uint32_t i = 0; i < width; ++i) put("-");
    put("\n");
}

static const char* svc_group_name(const char* name) {
    if (!name) return "other";
    if (starts_with(name, "drv.")) return "drivers";
    if (starts_with(name, "demo.") || starts_with(name, "app.") || streq(name, "msh")) return "programs";
    return "services";
}

static int cmd_help(int, char**) {
    putln("msh builtins:");
    for (uint32_t i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]); ++i) {
        put("  ");
        put(kBuiltins[i].name);
        put(" - ");
        putln(kBuiltins[i].help);
    }
    putln("");
    putln("Midix commands:");
    putln("  svc list | svc tree | svc groups | svc deps NAME");
    putln("  svc log NAME | svc start NAME | svc stop NAME | svc restart NAME");
    putln("  cap ls | cap tree | cap inspect HANDLE | cap revoke HANDLE");
    putln("  arena ls | arena show NAME");
    putln("  ring stats | doorbell stats");
    putln("  shutdown | reboot");
    putln("  vm list | vm start linux0 | vm console linux0");
    return 0;
}

static int cmd_exit(int argc, char**) {
    g.running = false;
    return argc > 1 ? 0 : 0;
}

static int cmd_echo(int argc, char** argv) {
    uint32_t start = 1;
    bool newline = true;
    if (argc > 1 && streq(argv[1], "-n")) {
        newline = false;
        start = 2;
    }
    for (uint32_t i = start; i < static_cast<uint32_t>(argc); ++i) {
        if (i != start) put(" ");
        put(argv[i]);
    }
    if (newline) put("\n");
    return 0;
}

static int cmd_printf(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i != 1) put(" ");
        const char* s = argv[i];
        for (uint32_t j = 0; s[j]; ++j) {
            if (s[j] == '\\' && s[j + 1]) {
                ++j;
                if (s[j] == 'n') put("\n");
                else if (s[j] == 't') put("\t");
                else put_bytes(&s[j], 1);
            } else {
                put_bytes(&s[j], 1);
            }
        }
    }
    return 0;
}

static int cmd_clear(int, char**) {
    for (uint32_t i = 0; i < 64; ++i) put("\n");
    return 0;
}

static int cmd_set(int argc, char** argv) {
    if (argc == 1) {
        for (uint32_t i = 0; i < kVarMax; ++i) {
            if (!g.vars[i].used) continue;
            put(g.vars[i].name);
            put("=");
            putln(g.vars[i].value);
        }
        return 0;
    }
    for (int a = 1; a < argc; ++a) {
        char* eq = nullptr;
        for (uint32_t i = 0; argv[a][i]; ++i) {
            if (argv[a][i] == '=') {
                eq = argv[a] + i;
                break;
            }
        }
        if (!eq) {
            put("msh: set: expected NAME=VALUE: ");
            putln(argv[a]);
            return 1;
        }
        *eq = '\0';
        if (!set_var(argv[a], eq + 1)) return 1;
        *eq = '=';
    }
    return 0;
}

static int cmd_unset(int argc, char** argv) {
    if (argc < 2) return 0;
    for (int a = 1; a < argc; ++a) {
        EnvVar* v = find_var(argv[a]);
        if (v) memzero(v, sizeof(*v));
    }
    return 0;
}

static int cmd_bootinfo(int, char**) {
    if (!g.boot || !mochi::boot::valid(g.boot)) return 1;
    put("BootInfo entries: "); put_dec(g.boot->entry_count); put("\n");
    for (uint16_t i = 0; i < g.boot->entry_count; ++i) {
        const MxBootEntry& e = g.boot->entries[i];
        put("  "); put_dec(i); put("  ");
        put(boot_type_name(e.type)); put("  ");
        put(e.name); put("  h="); put_hex(e.handle);
        if (e.aux0 || e.aux1) {
            put(" aux0="); put_hex(e.aux0); put(" aux1="); put_hex(e.aux1);
        }
        put("\n");
    }
    return 0;
}

static const char* svc_state_name(mx_u32_t state);

static void svc_print_section(const char* group_name) {
    put(group_name); put("\n");
    put_rule(73);
    put_padded("NAME", 24);
    put_padded("STATE", 10);
    put_padded("PRINCIPAL", 19);
    put_padded("RESTART", 10);
    putln("IMAGE");
    put_rule(73);
    if (!g.svc_registry) return;
    mochi::svc::RegistryEntry e{};
    const uint16_t count = svc_registry_entry_count();
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (!streq(svc_group_name(e.name), group_name)) continue;
        put_padded(e.name, 24);
        put_padded(svc_state_name(e.state), 10);
        put_padded(e.principal, 19);
        put_padded(e.restart, 10);
        putln(e.image);
    }
    put("\n");
}

static int svc_list() {
    if (!g.svc_registry) return notimpl("svc registry");
    svc_print_section("services");
    svc_print_section("drivers");
    svc_print_section("programs");
    return 0;
}

static int svc_groups() {
    putln("services  io.*, nameservice, tty.*, ui.*, dango.*");
    putln("drivers   drv.*");
    putln("programs  demo.*, app.*, msh");
    return 0;
}

static int svc_tree() {
    if (!g.svc_registry) return notimpl("svc registry");
    putln("userland");
    putln("├─ services");
    mochi::svc::RegistryEntry e{};
    const uint16_t count = svc_registry_entry_count();
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (streq(svc_group_name(e.name), "services")) { put("│  ├─ "); putln(e.name); }
    }
    putln("├─ drivers");
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (streq(svc_group_name(e.name), "drivers")) { put("│  ├─ "); putln(e.name); }
    }
    putln("└─ programs");
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (streq(svc_group_name(e.name), "programs")) { put("   ├─ "); putln(e.name); }
    }
    return 0;
}

static int svc_deps(const char* name) {
    if (!svc_exists(name)) { put("svc: not found: "); putln(name); return 1; }
    putln(name);
    if (streq(name, "msh")) {
        putln("├─ tty.fb0 terminal rings");
        putln("├─ io.fs command/reply rings");
        putln("└─ init svc.registry arena");
    } else if (streq(name, "tty.fb0")) {
        putln("├─ io.framebuffer");
        putln("├─ ui.input");
        putln("├─ io.console mirror");
        putln("└─ msh terminal rings");
    } else if (streq(name, "io.fs")) {
        putln("└─ drv.blk.virtio");
    } else if (streq(name, "dango.manager")) {
        putln("├─ vm.root");
        putln("└─ boot.initrd linux artifacts");
    } else {
        putln("└─ deps unavailable in static v0 registry");
    }
    return 0;
}

static const char* svc_state_name(mx_u32_t state) {
    if (state & mochi::svc::kStateRunning) return "running";
    if (state & mochi::svc::kStateSpawned) return "spawned";
    return "stopped";
}

static int svc_control(mx_u32_t op, const char* name, const char* verb) {
    if (!name) { put("usage: svc "); put(verb); putln(" NAME"); return 1; }
    if (!g.have_svcctl) return notimpl("svc control");
    mochi::svcctl::Reply reply{};
    if (!g.svcctl.request(op, name, &reply)) {
        putln("svc: control channel failure");
        return 1;
    }
    if (reply.status != MX_OK) {
        put("svc "); put(verb); put(" "); put(name); put(": "); put_status(reply.status); put("\n");
        return 1;
    }
    put("svc "); put(verb); put(" "); put(name); put(" -> "); putln(svc_state_name(reply.state));
    return 0;
}

static int system_control(mx_u32_t op, const char* verb) {
    if (!g.have_svcctl) return notimpl(verb);
    mochi::svcctl::Reply reply{};
    if (!g.svcctl.request(op, "system", &reply)) {
        put("msh: "); put(verb); putln(": control channel failure");
        return 1;
    }
    if (reply.status != MX_OK) {
        put("msh: "); put(verb); put(": "); put_status(reply.status); put("\n");
        return 1;
    }
    put(verb); putln(" requested");
    return 0;
}

static int cmd_shutdown(int, char**) {
    return system_control(mochi::svcctl::kRequestShutdown, "shutdown");
}

static int cmd_reboot(int, char**) {
    return system_control(mochi::svcctl::kRequestReboot, "reboot");
}

static int cmd_svc(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "list")) return svc_list();
    if (streq(argv[1], "tree")) return svc_tree();
    if (streq(argv[1], "groups")) return svc_groups();
    if (streq(argv[1], "deps")) {
        if (argc < 3) { putln("usage: svc deps NAME"); return 1; }
        return svc_deps(argv[2]);
    }
    if (streq(argv[1], "log")) return notimpl("svc log");
    if (streq(argv[1], "start")) return svc_control(mochi::svcctl::kRequestStart, argc >= 3 ? argv[2] : nullptr, "start");
    if (streq(argv[1], "stop")) return svc_control(mochi::svcctl::kRequestStop, argc >= 3 ? argv[2] : nullptr, "stop");
    if (streq(argv[1], "restart")) return svc_control(mochi::svcctl::kRequestRestart, argc >= 3 ? argv[2] : nullptr, "restart");
    putln("usage: svc list|tree|groups|deps NAME|log NAME|start NAME|stop NAME|restart NAME");
    return 1;
}

static void print_cap_info(mx_handle_t h) {
    MxCapInfo info{};
    mx_status_t st = mx_cap_info(h, &info);
    if (st != MX_OK) {
        put_status(st);
        return;
    }
    put(cap_type_name(info.type));
    put(" rights="); put_hex(info.rights);
    put(" base="); put_hex(info.base);
    put(" size="); put_hex(info.size);
    if (info.attr) { put(" attr="); put_hex(info.attr); }
    if (info.flags) { put(" flags="); put_hex(info.flags); }
    if (info.aux0) { put(" aux0="); put_hex(info.aux0); }
}

static int cmd_cap(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "ls")) {
        if (!g.boot) return 1;
        putln("BOOTINFO CAPABILITIES");
        for (uint16_t i = 0; i < g.boot->entry_count; ++i) {
            const MxBootEntry& e = g.boot->entries[i];
            if (e.handle == MX_NULL_HANDLE) continue;
            put("  "); put_dec(i); put(" "); put(e.name); put(" "); put_hex(e.handle); put(" ");
            print_cap_info(e.handle);
            put("\n");
        }
        return 0;
    }
    if (streq(argv[1], "tree")) {
        putln("msh cap tree: only BootInfo-visible caps are exposed in v0");
        return cmd_cap(1, argv);
    }
    if (streq(argv[1], "inspect")) {
        if (argc < 3) { putln("usage: cap inspect HANDLE"); return 1; }
        bool ok = false;
        mx_handle_t h = parse_u64(argv[2], &ok);
        if (!ok) { putln("cap inspect: bad handle"); return 1; }
        put_hex(h); put(" "); print_cap_info(h); put("\n");
        return 0;
    }
    if (streq(argv[1], "revoke")) {
        if (argc < 3) { putln("usage: cap revoke HANDLE"); return 1; }
        bool ok = false;
        mx_handle_t h = parse_u64(argv[2], &ok);
        if (!ok) { putln("cap revoke: bad handle"); return 1; }
        sys_cap_revoke(h);
        putln("revoked");
        return 0;
    }
    putln("usage: cap ls|tree|inspect HANDLE|revoke HANDLE");
    return 1;
}

static int cmd_arena(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "ls")) {
        if (!g.boot) return 1;
        putln("ARENAS");
        for (uint16_t i = 0; i < g.boot->entry_count; ++i) {
            const MxBootEntry& e = g.boot->entries[i];
            if (e.type != MX_BOOT_ENTRY_ARENA) continue;
            put("  "); put(e.name); put(" "); put_hex(e.handle); put(" "); print_cap_info(e.handle); put("\n");
        }
        return 0;
    }
    if (streq(argv[1], "show")) {
        if (argc < 3) { putln("usage: arena show NAME"); return 1; }
        if (g.svc_registry && svc_exists(argv[2])) {
            put(argv[2]); putln(": no per-service arena registry is exposed yet; boot arenas only:");
            char* fake_argv[2] = {argv[0], const_cast<char*>("ls")};
            return cmd_arena(2, fake_argv);
        }
        const MxBootEntry* e = mochi::boot::find_type(g.boot, argv[2], MX_BOOT_ENTRY_ARENA);
        if (!e) { put("arena not found: "); putln(argv[2]); return 1; }
        put(e->name); put(" "); put_hex(e->handle); put(" "); print_cap_info(e->handle); put("\n");
        return 0;
    }
    putln("usage: arena ls|show NAME");
    return 1;
}

static int cmd_ring_stats(int, char**) {
    if (!g.boot) return 1;
    putln("RINGS");
    for (uint16_t i = 0; i < g.boot->entry_count; ++i) {
        const MxBootEntry& e = g.boot->entries[i];
        if (e.type != MX_BOOT_ENTRY_RING) continue;
        put("  "); put(e.name); put(" order="); put_dec(e.aux0 ? e.aux0 : kDefaultOrder);
        put(" handle="); put_hex(e.handle); put(" "); print_cap_info(e.handle); put("\n");
    }
    return 0;
}

static int cmd_ring(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "stats")) return cmd_ring_stats(argc, argv);
    putln("usage: ring stats");
    return 1;
}

static int cmd_doorbell_stats(int, char**) {
    if (!g.boot) return 1;
    putln("DOORBELLS");
    for (uint16_t i = 0; i < g.boot->entry_count; ++i) {
        const MxBootEntry& e = g.boot->entries[i];
        if (e.type != MX_BOOT_ENTRY_DOORBELL) continue;
        put("  "); put(e.name); put(" handle="); put_hex(e.handle);
        put(" counter="); put_dec(mx_doorbell_read(e.handle));
        put(" "); print_cap_info(e.handle); put("\n");
    }
    return 0;
}

static int cmd_doorbell(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "stats")) return cmd_doorbell_stats(argc, argv);
    putln("usage: doorbell stats");
    return 1;
}

static int cmd_vm(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "list")) {
        putln("NAME     STATE       DETAIL");
        put("linux0   ");
        const int64_t created = sys_debug_counter(dango::VM_DEBUG_CREATED);
        const int64_t channels = sys_debug_counter(dango::VM_DEBUG_CHANNELS_CREATED);
        if (created > 0) put("created     "); else put("uncreated   ");
        put("created="); put_dec(created > 0 ? static_cast<uint64_t>(created) : 0);
        put(" channels="); put_dec(channels > 0 ? static_cast<uint64_t>(channels) : 0);
        put("\n");
        return 0;
    }
    if (streq(argv[1], "start")) {
        putln("vm start: dango.manager in this tree starts linux0 during boot; no runtime VM manager command ring exists yet");
        return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
    }
    if (streq(argv[1], "console")) {
        putln("vm console: VM channel exists in kernel/dango, but no dango.manager console attach protocol exists yet");
        return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
    }
    putln("usage: vm list|start linux0|console linux0");
    return 1;
}

static int execute_simple(int argc, char** argv) {
    if (argc <= 0) return 0;

    // Leading assignments in a simple command mutate shell vars for now.
    uint32_t first = 0;
    while (first < static_cast<uint32_t>(argc) && token_is_assignment(argv[first])) {
        char* eq = nullptr;
        for (uint32_t i = 0; argv[first][i]; ++i) {
            if (argv[first][i] == '=') { eq = argv[first] + i; break; }
        }
        if (eq) {
            *eq = '\0';
            (void)set_var(argv[first], eq + 1);
            *eq = '=';
        }
        ++first;
    }
    if (first >= static_cast<uint32_t>(argc)) return 0;

    for (uint32_t i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]); ++i) {
        if (streq(argv[first], kBuiltins[i].name)) {
            return kBuiltins[i].fn(argc - static_cast<int>(first), argv + first);
        }
    }
    put("msh: command not found: ");
    putln(argv[first]);
    return 127;
}

static int execute_parsed(ParsedLine* p) {
    if (!p) return 1;
    int status = 0;
    char* argv[kArgMax];
    int argc = 0;
    enum NextMode { Always, IfSuccess, IfFailure } next = Always;
    bool should_run = true;

    auto flush = [&]() -> void {
        if (argc == 0) return;
        if (should_run) status = execute_simple(argc, argv);
        argc = 0;
    };

    for (uint32_t i = 0; i <= p->count; ++i) {
        const bool at_end = i == p->count;
        const bool control = !at_end && p->tokens[i].control;
        if (!at_end && !control) {
            if (argc < static_cast<int>(kArgMax)) argv[argc++] = p->tokens[i].text;
            continue;
        }
        flush();
        if (at_end) break;
        if (streq(p->tokens[i].text, ";")) {
            next = Always;
            should_run = true;
        } else if (streq(p->tokens[i].text, "&&")) {
            next = IfSuccess;
            should_run = (status == 0);
        } else if (streq(p->tokens[i].text, "||")) {
            next = IfFailure;
            should_run = (status != 0);
        } else {
            (void)next;
        }
    }
    return status;
}

static void prompt() {
    char buf[kPromptMax];
    build_prompt(buf, sizeof(buf));
    put(buf);
}

extern "C" [[noreturn]] void _start() {
    g.running = true;
    g.last_status = 0;
    g.cwd_handle = 1;
    copy_str(g.cwd, sizeof(g.cwd), "/");
    set_var("PWD", "/");
    set_var("SHELL", "msh");

    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    g.boot = boot;
    const bool log_ready = mochi::log_map_from_boot(boot, kVaLogRing, "msh");
    const bool term_ready = boot_map_term(boot);
    const bool fs_ready = boot_map_fs(boot);
    (void)boot_map_svc_registry(boot);
    (void)boot_map_svcctl(boot);
    if (log_ready) {
        mochi::log_raw_marker(term_ready
            ? "msh: terminal attached\n"
            : "msh: terminal missing; using debug console fallback\n");
        if (!fs_ready) {
            mochi::log_raw_marker("msh: fs endpoint missing\n");
        }
    }

    putln("msh: Midix shell ready (libmochi BootInfo/rings/caps)");
    putln("type `help` for commands");

    while (g.running) {
        prompt();
        if (!read_line(g_line, sizeof(g_line))) break;
        if (!parse_line(g_line, &g_parsed)) {
            g.last_status = 2;
            continue;
        }
        g.last_status = execute_parsed(&g_parsed);
    }
    putln("logout");
    mx_exit(g.last_status);
    while (true) mx_yield();
}
