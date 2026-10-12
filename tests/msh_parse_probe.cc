#include <cassert>
#include "../userland/msh/msh_parse.cc"

Shell g{};
static unsigned executed;
static int record(int, char**) { ++executed; return 0; }
const Builtin kBuiltins[] = {{"record", record, ""}};
const uint32_t kBuiltinCount = 1;
bool streq(const char* a, const char* b) { return libk::str::equal(a, b); }
bool is_space(char c) { return libk::str::is_space(c); }
bool is_name_char(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || libk::str::is_digit(c);
}
void put(const char*) {}
void putln(const char*) {}

int main() {
    ParsedLine parsed{};
    assert(set_var("name", "before"));
    char oversized[kVarValueMax + 1];
    libk::fill(libk::as_bytes(oversized), 'x');
    oversized[kVarValueMax] = 0;
    assert(!set_var("name", oversized));
    assert(streq(find_var("name")->value, "before"));
    char long_name[kVarNameMax + 1];
    libk::fill(libk::as_bytes(long_name), 'x');
    long_name[kVarNameMax] = 0;
    assert(!set_var(long_name, "alias"));
    assert(parse_line("record 'a b' \"$name\"; record && record", &parsed));
    assert(streq(parsed.tokens[1].text, "a b") && streq(parsed.tokens[2].text, "before"));
    assert(execute_parsed(&parsed) == 0 && executed == 3);
    g.last_status = -17;
    assert(parse_line("record $?", &parsed) && streq(parsed.tokens[1].text, "-17"));
    assert(!parse_line(oversized, &parsed));
    char expansion[kVarNameMax + 2];
    expansion[0] = '$';
    libk::str::copy_into(expansion + 1, sizeof(expansion) - 1, long_name);
    assert(!parse_line(expansion, &parsed));
    char many[256];
    libk::str::Buffer words(many);
    words.append("record");
    for (unsigned i = 0; i < kArgMax; ++i) words.append(" arg");
    assert(!parse_line(many, &parsed));
    assert(executed == 3);
    char assignment[sizeof(long_name) + 8];
    libk::str::Buffer text(assignment);
    text.append(long_name); text.append("=value");
    char command[] = "record";
    char* argv[] = {assignment, command};
    assert(execute_simple(2, argv) != 0 && executed == 3);
    assert(parse_line("record a; record b", &parsed));
    assert(execute_parsed(&parsed) == 0 && executed == 5);
}
