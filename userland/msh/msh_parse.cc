// userland/msh/msh_parse.cc

#include "msh.h"

#include "libk/printf.h"

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
        (void)libk::snprintf(last, sizeof(last), "%lld",
                             static_cast<long long>(g.last_status));
        return last;
    }
    if (streq(name, "PWD")) return g.cwd;
    EnvVar* v = find_var(name);
    return v ? v->value : "";
}

bool set_var(const char* name, const char* value) {
    if (!name || !name[0] || libk::str::length_n(name, kVarNameMax) >= kVarNameMax) return false;
    if (!value) value = "";
    if (libk::str::length_n(value, kVarValueMax) >= kVarValueMax) return false;
    EnvVar* v = find_var(name);
    if (!v) {
        for (uint32_t i = 0; i < kVarMax; ++i) {
            if (!g.vars[i].used) {
                v = &g.vars[i];
                v->used = true;
                libk::str::copy_into(v->name, sizeof(v->name), name);
                break;
            }
        }
    }
    if (!v) return false;
    libk::str::copy_into(v->value, sizeof(v->value), value ? value : "");
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
    uint32_t arguments = 0;
    while (line && line[i]) {
        while (is_space(line[i])) ++i;
        if (!line[i]) break;
        if (line[i] == '#') break;
        if (out->count >= kTokenMax) return false;

        Token& t = out->tokens[out->count];
        libk::zero(t);
        libk::str::Buffer text(t.text);

        // A control operator is its own token: ; && ||.
        const auto control = [&](const char* op, uint32_t advance) {
            arguments = 0;
            t.control = true;
            text.append(op);
            i += advance;
            ++out->count;
        };
        if (line[i] == ';') { control(";", 1); continue; }
        if (line[i] == '&' && line[i + 1] == '&') { control("&&", 2); continue; }
        if (line[i] == '|' && line[i + 1] == '|') { control("||", 2); continue; }

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
                text.append(line[i + 1]);
                i += 2;
                continue;
            }
            if (!single && c == '$') {
                ++i;
                char name[kVarNameMax];
                libk::str::Buffer variable(name);
                if (line[i] == '?') {
                    variable.append('?');
                    ++i;
                } else {
                    while (is_name_char(line[i])) {
                        variable.append(line[i]);
                        ++i;
                    }
                }
                if (variable.truncated) {
                    putln("msh: variable name too long");
                    return false;
                }
                text.append(get_var(name));
                continue;
            }
            text.append(c);
            ++i;
        }
        if (single || dbl) {
            putln("msh: unmatched quote");
            return false;
        }
        if (text.truncated || ++arguments > kArgMax) {
            putln(text.truncated ? "msh: expanded word too long" : "msh: too many arguments");
            return false;
        }
        ++out->count;
    }
    return true;
}


bool set_var_from_assignment(char* token) {
    if (!token) return false;
    char* eq = nullptr;
    for (uint32_t i = 0; token[i]; ++i) {
        if (token[i] == '=') { eq = token + i; break; }
    }
    if (!eq) return false;
    *eq = '\0';
    const bool ok = set_var(token, eq + 1);
    *eq = '=';
    return ok;
}

int execute_simple(int argc, char** argv) {
    if (argc <= 0) return 0;

    // Leading assignments in a simple command mutate shell vars for now.
    uint32_t first = 0;
    while (first < static_cast<uint32_t>(argc) && token_is_assignment(argv[first])) {
        if (!set_var_from_assignment(argv[first])) {
            putln("msh: cannot store variable");
            return 1;
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
    if (!p || p->count > kTokenMax) return 1;
    // Validate the whole command before dispatch: never execute a shortened argv.
    uint32_t words = 0;
    for (uint32_t i = 0; i < p->count; ++i) {
        if (p->tokens[i].control) words = 0;
        else if (++words > kArgMax) return 1;
    }
    int status = 0;
    char* argv[kArgMax];
    int argc = 0;
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
            argv[argc++] = p->tokens[i].text;
            continue;
        }
        flush();
        if (at_end) break;
        if (streq(p->tokens[i].text, ";")) {
            should_run = true;
        } else if (streq(p->tokens[i].text, "&&")) {
            should_run = (status == 0);
        } else if (streq(p->tokens[i].text, "||")) {
            should_run = (status != 0);
        }
    }
    return status;
}

