#pragma once
// Allocation-free JSON views. Inputs are length-delimited; duplicate keys,
// excessive nesting, embedded NULs, and unsupported numeric forms are rejected.
#include <cstdint>
#include <cstddef>
namespace midix::json {
struct Value {
    const char* begin = nullptr;
    const char* end = nullptr;
    explicit operator bool() const {
        return begin && end && begin < end;
    }
};
inline void ws(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        ++p;
    }
}
inline int hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
inline bool character(const char*& p, const char* end, uint32_t& c) {
    if (p == end) {
        return false;
    }
    c = static_cast<unsigned char>(*p++);
    if (c < 32 || c == '"') {
        return false;
    }
    if (c != '\\') {
        return true;
    }
    if (p == end) {
        return false;
    }
    switch (*p++) {
    case '"':
        c = '"';
        return true;
    case '\\':
        c = '\\';
        return true;
    case '/':
        c = '/';
        return true;
    case 'b':
        c = 8;
        return true;
    case 'f':
        c = 12;
        return true;
    case 'n':
        c = 10;
        return true;
    case 'r':
        c = 13;
        return true;
    case 't':
        c = 9;
        return true;
    case 'u':
        c = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (p == end || hex(*p) < 0) {
                return false;
            }
            c = (c << 4) | static_cast<unsigned>(hex(*p++));
        }
        return c && !(c >= 0xd800 && c <= 0xdfff);
    default:
        return false;
    }
}
inline bool string_token(const char*& p, const char* end) {
    if (p == end || *p++ != '"') {
        return false;
    }
    while (p < end && *p != '"') {
        uint32_t c;
        if (!character(p, end, c)) {
            return false;
        }
    }
    if (p == end) {
        return false;
    }
    ++p;
    return true;
}
inline bool same_string(Value a, Value b) {
    if (!a || !b || *a.begin != '"' || *b.begin != '"') {
        return false;
    }
    const char* p = a.begin + 1;
    const char* q = b.begin + 1;
    while (p < a.end - 1 && q < b.end - 1) {
        uint32_t x, y;
        if (!character(p, a.end - 1, x) || !character(q, b.end - 1, y) || x != y) {
            return false;
        }
    }
    return p == a.end - 1 && q == b.end - 1;
}
inline bool skip(const char*& p, const char* end, unsigned depth = 0,
                 bool check_duplicates = true) {
    ws(p, end);
    if (p == end || depth > 16) {
        return false;
    }
    if (*p == '"') {
        return string_token(p, end);
    }
    if (*p == '{' || *p == '[') {
        const bool object = *p++ == '{';
        const char close = object ? '}' : ']';
        const char* start = p;
        ws(p, end);
        if (p < end && *p == close) {
            ++p;
            return true;
        }
        while (p < end) {
            if (object) {
                const char* key_start = p;
                if (!string_token(p, end)) {
                    return false;
                }
                const Value key{key_start, p};
                // Compare against preceding keys, decoding escapes identically.
                const char* q = start;
                ws(q, key_start);
                while (check_duplicates && q < key_start) {
                    const char* old = q;
                    if (!string_token(q, key_start)) {
                        return false;
                    }
                    if (same_string(key, {old, q})) {
                        return false;
                    }
                    ws(q, key_start);
                    if (q == key_start || *q++ != ':') {
                        return false;
                    }
                    if (!skip(q, key_start, depth + 1, false)) {
                        return false;
                    }
                    ws(q, key_start);
                    if (q < key_start && *q == ',') {
                        ++q;
                    }
                    ws(q, key_start);
                }
                ws(p, end);
                if (p == end || *p++ != ':') {
                    return false;
                }
            }
            if (!skip(p, end, depth + 1, check_duplicates)) {
                return false;
            }
            ws(p, end);
            if (p == end) {
                return false;
            }
            if (*p == close) {
                ++p;
                return true;
            }
            if (*p++ != ',') {
                return false;
            }
            ws(p, end);
        }
        return false;
    }
    if (*p >= '0' && *p <= '9') {
        const bool zero = *p == '0';
        ++p;
        if (zero && p < end && *p >= '0' && *p <= '9') {
            return false;
        }
        while (p < end && *p >= '0' && *p <= '9') {
            ++p;
        }
        return true;
    }
    const char* words[] = {"true", "false", "null"};
    for (const char* word : words) {
        const char* q = p;
        const char* w = word;
        while (*w && q < end && *q == *w) {
            ++q;
            ++w;
        }
        if (!*w) {
            p = q;
            return true;
        }
    }
    return false;
}
inline Value parse(const char* data, size_t length) {
    if (!data || length > 65536) {
        return {};
    }
    const char* p = data;
    const char* end = data + length;
    ws(p, end);
    const char* start = p;
    if (!skip(p, end)) {
        return {};
    }
    const char* stop = p;
    ws(p, end);
    return p == end ? Value{start, stop} : Value{};
}
inline bool string(Value v, char* out, size_t cap) {
    if (!v || *v.begin != '"' || !cap) {
        return false;
    }
    const char* p = v.begin + 1;
    size_t used = 0;
    while (p < v.end - 1) {
        uint32_t c;
        if (!character(p, v.end - 1, c) || !c || c > 127 || used + 1 >= cap) {
            return false;
        }
        out[used++] = static_cast<char>(c);
    }
    out[used] = 0;
    return true;
}
inline bool equals(Value v, const char* name) {
    if (!v || *v.begin != '"') {
        return false;
    }
    const char* p = v.begin + 1;
    while (p < v.end - 1) {
        uint32_t c;
        if (!*name || !character(p, v.end - 1, c) || c != static_cast<unsigned char>(*name++)) {
            return false;
        }
    }
    return !*name;
}
inline Value member(Value object, const char* name) {
    if (!object || *object.begin != '{') {
        return {};
    }
    const char* p = object.begin + 1;
    while (p < object.end) {
        ws(p, object.end);
        if (p == object.end || *p == '}') {
            break;
        }
        const char* key = p;
        if (!string_token(p, object.end)) {
            return {};
        }
        Value k{key, p};
        ws(p, object.end);
        if (p == object.end || *p++ != ':') {
            return {};
        }
        ws(p, object.end);
        const char* start = p;
        if (!skip(p, object.end, 0, false)) {
            return {};
        }
        if (equals(k, name)) {
            return {start, p};
        }
        ws(p, object.end);
        if (p < object.end && *p == ',') {
            ++p;
        }
    }
    return {};
}
inline Value element(Value array, size_t index) {
    if (!array || *array.begin != '[') {
        return {};
    }
    const char* p = array.begin + 1;
    for (size_t i = 0; p < array.end; ++i) {
        ws(p, array.end);
        if (p == array.end || *p == ']') {
            return {};
        }
        const char* start = p;
        if (!skip(p, array.end, 0, false)) {
            return {};
        }
        if (i == index) {
            return {start, p};
        }
        ws(p, array.end);
        if (p < array.end && *p == ',') {
            ++p;
        }
    }
    return {};
}
inline bool number(Value v, uint64_t& out) {
    if (!v) {
        return false;
    }
    out = 0;
    for (const char* p = v.begin; p < v.end; ++p) {
        if (*p < '0' || *p > '9' || out > (~uint64_t{0} - (*p - '0')) / 10) {
            return false;
        }
        out = out * 10 + (*p - '0');
    }
    return true;
}
} // namespace midix::json
