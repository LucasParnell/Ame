// userland/msh/msh_line.cc

#include "msh.h"

// Interactive line editing: prompt, history ring, terminal rendering,
// completion, and the blocking read_line() loop.  All terminal state lives in
// the shared Shell; the history ring is private to this module.

// History is only touched by this module (push on submit, recall on the arrows).
static char g_history[kHistoryMax][kLineMax];
static uint32_t g_history_count = 0;
static uint32_t g_history_head = 0;

uint32_t build_prompt(char* out, uint32_t cap) {
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

// Control bytes the line editor recognises, named so the loop reads as keys
// rather than numbers.
constexpr char kEsc   = 27;
constexpr char kCtrlA = 1, kCtrlB = 2, kCtrlC = 3, kCtrlD = 4, kCtrlE = 5, kCtrlF = 6;
constexpr char kCtrlK = 11, kCtrlL = 12, kCtrlN = 14, kCtrlP = 16, kCtrlU = 21;
constexpr char kDel = 0x7f;
constexpr uint32_t kClearLines = 48;

bool read_line(char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uint32_t len = 0;
    uint32_t cursor = 0;
    uint32_t prev_render_len = 0;
    char prompt_text[kPromptMax];
    build_prompt(prompt_text, sizeof(prompt_text));
    int32_t history_nav = -1;   // -1: editing a fresh line; >=0: history offset
    char live_line[kLineMax]{};

    const auto redraw = [&] { render_edit_line(prompt_text, out, len, cursor, &prev_render_len); };

    const auto load_history = [&](int32_t nav) {
        if (g_history_count == 0) return;
        if (nav < 0) nav = 0;
        if (nav >= static_cast<int32_t>(g_history_count)) nav = static_cast<int32_t>(g_history_count) - 1;
        const char* entry = history_get(g_history_count - 1u - static_cast<uint32_t>(nav));
        if (!entry) return;
        copy_str(out, cap, entry);
        len = slen(out);
        cursor = len;
        history_nav = nav;
        redraw();
    };

    // Up / down through history: shared by the arrow keys and ^P / ^N.
    const auto history_prev = [&] {
        if (history_nav < 0) copy_str(live_line, sizeof(live_line), out);
        load_history(history_nav < 0 ? 0 : history_nav + 1);
    };
    const auto history_next = [&] {
        if (history_nav > 0) {
            load_history(history_nav - 1);
        } else if (history_nav == 0) {
            copy_str(out, cap, live_line);
            len = slen(out);
            cursor = len;
            history_nav = -1;
            redraw();
        }
    };

    // Forward-delete at the cursor: shared by ^D and the ESC[3~ sequence.
    const auto delete_at_cursor = [&] {
        if (cursor >= len) return;
        for (uint32_t j = cursor; j < len; ++j) out[j] = out[j + 1];
        --len;
        redraw();
    };

    // Escape (arrow / function key) sequence state.
    enum EscState { EscNone, EscSeen, EscCsi, EscParam };
    EscState esc = EscNone;
    char esc_param = '\0';

    for (;;) {
        if (!g.have_terminal || !g.term.valid()) return false;
        RingSlot slot{};
        if (!g.term.wait_read(&slot)) return false;
        if (slot.type != MX_TERM_INPUT_BYTES) continue;

        for (uint32_t i = 0; i < slot.length; ++i) {
            char c = static_cast<char>(slot.payload[i]);

            if (esc == EscSeen) {
                if (c == '[') { esc = EscCsi; continue; }
                esc = EscNone;   // a bare ESC followed by a key: dispatch that key
            } else if (esc == EscCsi) {
                if (c == 'A') { history_prev(); esc = EscNone; continue; }
                if (c == 'B') { history_next(); esc = EscNone; continue; }
                if (c == 'C') { if (cursor < len) { ++cursor; redraw(); } esc = EscNone; continue; }
                if (c == 'D') { if (cursor > 0) { --cursor; redraw(); } esc = EscNone; continue; }
                if (c == 'H') { cursor = 0; redraw(); esc = EscNone; continue; }
                if (c == 'F') { cursor = len; redraw(); esc = EscNone; continue; }
                if (c >= '0' && c <= '9') { esc_param = c; esc = EscParam; continue; }
                esc = EscNone;
            } else if (esc == EscParam) {
                if (esc_param == '3' && c == '~') delete_at_cursor();
                esc = EscNone;
                continue;
            }

            if (c == kEsc) { esc = EscSeen; continue; }
            if (c == '\r') c = '\n';

            switch (c) {
            case kCtrlP: history_prev(); continue;
            case kCtrlN: history_next(); continue;
            case kCtrlD:
                if (len == 0) return false;
                delete_at_cursor();
                continue;
            case kCtrlC: put("^C\n"); out[0] = '\0'; return true;
            case kCtrlA: cursor = 0; redraw(); continue;
            case kCtrlE: cursor = len; redraw(); continue;
            case kCtrlB: if (cursor > 0) { --cursor; redraw(); } continue;
            case kCtrlF: if (cursor < len) { ++cursor; redraw(); } continue;
            case kCtrlU:
                for (uint32_t j = cursor; j <= len; ++j) out[j - cursor] = out[j];
                len -= cursor;
                cursor = 0;
                redraw();
                continue;
            case kCtrlK: out[cursor] = '\0'; len = cursor; redraw(); continue;
            case kCtrlL: for (uint32_t j = 0; j < kClearLines; ++j) put("\n"); redraw(); continue;
            case '\b':
            case kDel:
                if (cursor > 0) {
                    for (uint32_t j = cursor - 1; j < len; ++j) out[j] = out[j + 1];
                    --cursor;
                    --len;
                    redraw();
                }
                continue;
            case '\t':
                (void)try_complete_line(out, cap, &len, &cursor, prompt_text, &prev_render_len);
                continue;
            case '\n':
                put("\n");
                out[len] = '\0';
                history_push(out);
                return true;
            default:
                break;
            }

            if (is_printable(c) && len + 1 < cap) {
                for (uint32_t j = len + 1; j > cursor; --j) out[j] = out[j - 1];
                out[cursor] = c;
                ++cursor;
                ++len;
                out[len] = '\0';
                history_nav = -1;
                redraw();
            }
        }
    }
}
