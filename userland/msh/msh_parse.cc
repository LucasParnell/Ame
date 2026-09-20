// userland/msh/msh_parse.cc

#include "msh.h"

// Shell variables, the word/token tokenizer, and command execution.
// The tokenizer expands $name and $? while it scans; execution splits a
// parsed line on the control operators ; && || and dispatches to the builtin
// registry.

EnvVar* find_var(const char* name) {
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

bool set_var(const char* name, const char* value) {
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


bool parse_line(const char* line, ParsedLine* out) {
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


int execute_simple(int argc, char** argv) {
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

    for (uint32_t i = 0; i < kBuiltinCount; ++i) {
        if (streq(argv[first], kBuiltins[i].name)) {
            return kBuiltins[i].fn(argc - static_cast<int>(first), argv + first);
        }
    }
    put("msh: command not found: ");
    putln(argv[first]);
    return 127;
}

int execute_parsed(ParsedLine* p) {
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

