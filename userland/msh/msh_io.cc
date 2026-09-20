// userland/msh/msh_io.cc

#include "msh.h"

// Output, string helpers, cap/boot type names, and service-registry reads.

uint32_t slen(const char* s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
}

bool svc_registry_valid_snapshot(const mochi::svc::RegistryPage* page) {
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

uint16_t svc_registry_entry_count() {
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

bool svc_registry_copy_entry(uint16_t index, mochi::svc::RegistryEntry* out) {
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

bool svc_exists(const char* name) {
    mochi::svc::RegistryEntry e{};
    const uint16_t count = svc_registry_entry_count();
    for (uint16_t i = 0; i < count; ++i) {
        if (svc_registry_copy_entry(i, &e) && streq(e.name, name)) return true;
    }
    return false;
}

void memzero(void* p, uint64_t len) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < len; ++i) b[i] = 0;
}

void copy_str(char* dst, uint32_t cap, const char* src) {
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

void append_char(char* dst, uint32_t cap, uint32_t* n, char c) {
    if (!dst || !n || cap == 0) return;
    if (*n + 1 < cap) {
        dst[*n] = c;
        ++*n;
        dst[*n] = '\0';
    }
}

void append_str(char* dst, uint32_t cap, uint32_t* n, const char* s) {
    if (!s) s = "";
    while (*s) append_char(dst, cap, n, *s++);
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_printable(char c) {
    return c >= 32 && c <= 126;
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

uint64_t parse_u64(const char* s, bool* ok) {
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

void u64_to_dec(uint64_t v, char* out, uint32_t cap) {
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

void u64_to_hex(uint64_t v, char* out, uint32_t cap) {
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

void put_bytes(const char* data, uint32_t len) {
    if (!data || len == 0) return;
    if (g.have_terminal && g.term.valid()) {
        (void)g.term.write_bytes_spinning(data, len, 0xFFFFu);
    } else {
        sys_debug_puts(data, len);
    }
}

void put(const char* s) {
    put_bytes(s, slen(s));
}

void putln(const char* s) {
    put(s);
    put("\n");
}

void put_dec(uint64_t v) {
    char b[32];
    u64_to_dec(v, b, sizeof(b));
    put(b);
}

void put_hex(uint64_t v) {
    char b[32];
    u64_to_hex(v, b, sizeof(b));
    put(b);
}

void put_status(mx_status_t s) {
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

const char* cap_type_name(uint32_t type) {
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

const char* boot_type_name(uint32_t type) {
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


bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    uint32_t i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return false;
        ++i;
    }
    return true;
}


int notimpl(const char* what) {
    put("msh: ");
    put(what);
    putln(": service API not present in current libmochi/Ame tree");
    return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
}

