// userland/msh/msh_io.cc

#include "msh.h"

#include "libk/bytes.h"
#include "libk/num.h"
#include "libk/integer.h"
#include "libk/printf.h"
#include "libk/str.h"

// Output, string helpers, cap/boot type names, and service-registry reads.

uint32_t slen(const char* s) {
    return static_cast<uint32_t>(libk::str::length(s));
}

bool streq(const char* a, const char* b) {
    return libk::str::equal(a, b);
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

bool is_space(char c) {
    return libk::str::is_space(c);
}

bool is_printable(char c) {
    return libk::str::is_ascii_printable(c);
}

bool is_digit(char c) {
    return libk::str::is_digit(c);
}

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

uint64_t parse_u64(const char* s, bool* ok) {
    uint64_t value = 0;
    const bool valid = libk::num::parse_u64(s, &value);
    if (ok) *ok = valid;
    return value;
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
    char b[24];
    const int n = libk::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(v));
    put_bytes(b, n < 0 ? 0u : static_cast<uint32_t>(n));
}

void put_hex(uint64_t v) {
    char b[24];
    const int n = libk::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    put_bytes(b, n < 0 ? 0u : static_cast<uint32_t>(n));
}

void put_status(mx_status_t s) {
    if (s == MX_OK) { put("OK"); return; }
    put("err(");
    if (s < 0) {
        put("-");
        put_dec(libk::magnitude(s));
    } else {
        put_dec(static_cast<uint64_t>(s));
    }
    put(")");
}

const char* cap_type_name(uint32_t type) {
    // Names live next to the capability enum in the ABI (midix/abi/cap.h).
    return mx_cap_type_name(type);
}

const char* boot_type_name(uint32_t type) {
    // Names live next to the BootInfo entry enum in the ABI (midix/abi/boot.h).
    return mx_boot_entry_type_name(type);
}


bool starts_with(const char* s, const char* prefix) {
    return libk::str::starts_with(s, prefix);
}


int notimpl(const char* what) {
    put("msh: ");
    put(what);
    putln(": service API not present in current libmochi/Ame tree");
    return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
}

