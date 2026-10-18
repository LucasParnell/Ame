#include <cassert>
#include <cstring>
#include <string>
#include "../userland/msh/msh_line.cc"
Shell g{};
const Builtin kBuiltins[] = {{"echo", nullptr, ""}};
const uint32_t kBuiltinCount = 1;
static std::string output;
uint32_t slen(const char* s) {
    return static_cast<uint32_t>(std::strlen(s));
}
bool streq(const char* a, const char* b) {
    return !std::strcmp(a, b);
}
bool starts_with(const char* a, const char* b) {
    return !std::strncmp(a, b, std::strlen(b));
}
bool is_space(char c) {
    return libk::str::is_space(c);
}
bool is_printable(char c) {
    return c >= 32 && c <= 126;
}
void put(const char* s) {
    output += s;
}
void put_bytes(const char* s, uint32_t n) {
    output.append(s, n);
}
uint16_t svc_registry_entry_count() {
    return 0;
}
bool svc_registry_copy_entry(uint16_t, mochi::svc::RegistryEntry*) {
    return false;
}
struct Storage {
    MxRingPage page{};
    MxRingSlot slots[8]{};
} incoming, outgoing;
static void send(const char* s) {
    assert(g.term.endpoint.rx().push_bytes(MX_TERM_INPUT_BYTES, s, std::strlen(s)) ==
           std::strlen(s));
}
static void line(const char* expected) {
    char b[256];
    assert(read_line(b, sizeof(b)));
    assert(!std::strcmp(b, expected));
}
int main() {
    g.have_terminal = true;
    g.term.endpoint = {&outgoing.page, &incoming.page, 3, 1, 1};
    std::strcpy(g.cwd, "/");
    send("ordinary-typing\n");
    line("ordinary-typing");
    assert(output == "ordinary-typing\r\n"); // one output byte per appended input byte
    output.clear();
    send("ac\x1b[Db\x1b[F!\n");
    line("abc!"); // insertion still repaints and appending after navigation works
    output.clear();
    send("first\r\nsecond\nthird\r");
    line("first");
    line("second");
    line("third");
    send("\nfourth\n");
    line("fourth");
    send("old-partial\x1b[");
    const MxTermControl off{MX_TERM_CONTROL_FLOW_OFF, 0, 0, 0},
        on{MX_TERM_CONTROL_FLOW_ON, 42, 0, 0};
    assert(g.term.endpoint.rx().try_push_msg(MX_TERM_CONTROL, off));
    assert(g.term.endpoint.rx().try_push_msg(MX_TERM_CONTROL, on));
    send("new-session\n");
    line("new-session");
    assert(output.find("msh:/$ ") != std::string::npos);
    send("cancel\x03next\n");
    line("");
    line("next");
    std::puts("msh stream input probe ok");
}
