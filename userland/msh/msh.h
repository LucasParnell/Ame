#pragma once
// userland/msh/msh.h — internal interface shared by the msh modules.
//
// msh is one freestanding image split into cohesive translation units:
//
//   msh_io.cc       output, string helpers, cap/boot type names, the
//                   service-registry snapshot readers, and notimpl()
//   msh_line.cc     prompt, history, line editing, completion, read_line
//   msh_parse.cc    shell variables, the tokenizer, and command execution
//   msh_builtins.cc the builtin registry and the core builtins
//   msh_fs.cc       io.fs paths and the filesystem builtins
//   msh_svc.cc      BootInfo, service, capability, ring, doorbell, VM tools
//   msh_diag.cc     serial diagnostic report and retained boot log replay
//   msh_power.cc    shutdown and reboot requests to init
//   main.cc         globals, BootInfo wiring, and _start()
//
// Everything declared here is private to msh; the modules are linked into a
// single image.  The split is a move of existing code, not a redesign.

#include <cstdint>

#include "libk/bytes.h"
#include "libk/str.h"
#include "libk/text.h"

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
#include "mochi/va_layout.h"
using mochi::va::msh::kVaBootInfo;
using mochi::va::msh::kVaLogRing;
using mochi::va::msh::kVaTermInput;
using mochi::va::msh::kVaTermOutput;
using mochi::va::msh::kVaFsCmdRing;
using mochi::va::msh::kVaFsReplyRing;
using mochi::va::msh::kVaSvcCmdRing;
using mochi::va::msh::kVaSvcReplyRing;
using mochi::va::msh::kVaSvcRegistry;

inline constexpr uint32_t kDefaultOrder      = MX_DEFAULT_RING_ORDER;
inline constexpr uint32_t kLineMax           = 256;
inline constexpr uint32_t kTokenMax          = 32;
inline constexpr uint32_t kArgMax            = 16;
inline constexpr uint32_t kVarMax            = 32;
inline constexpr uint32_t kVarNameMax        = 24;
inline constexpr uint32_t kVarValueMax       = 128;
inline constexpr uint32_t kHistoryMax        = 16;
inline constexpr uint32_t kPromptMax         = 96;
inline constexpr uint32_t kCompletionMax     = 24;
// Path handling limits (fs_internal.h's inline-name width and the component
// depth cap) and the well-known root handle.
inline constexpr uint32_t kPathNameMax  = 36;
inline constexpr uint32_t kPathDepthMax = 12;
inline constexpr uint32_t kPathMax      = 128;
inline constexpr fs_handle_t kRootHandle = 1;

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
    mochi::SpscEndpoint app_control;
    const mochi::svc::RegistryPage* svc_registry;
    EnvVar vars[kVarMax];
    char cwd[128];
    fs_handle_t cwd_handle;
    int32_t last_status;
    bool running;
    bool have_terminal;
    bool have_fs;
    bool have_svcctl;
    bool output_failed; // Sticky until the next diagnostic report starts.
};

struct Builtin {
    const char* name;
    int (*fn)(int, char**);
    const char* help;
};
int cmd_container(int argc, char** argv);

// Defined in main.cc: the interactive line buffer and parsed token block live
// outside the stack because an initial userland thread only has 4 KiB.
extern Shell g;
extern char g_line[kLineMax];
extern ParsedLine g_parsed;

// Defined in msh_builtins.cc.
extern const Builtin kBuiltins[];
extern const uint32_t kBuiltinCount;

// --- msh_io.cc ------------------------------------------------------------
uint32_t slen(const char* s);
bool streq(const char* a, const char* b);
bool starts_with(const char* s, const char* prefix);
bool is_space(char c);
bool is_printable(char c);
bool is_digit(char c);
bool is_name_char(char c);
uint64_t parse_u64(const char* s, bool* ok = nullptr);

void put_bytes(const char* data, uint32_t len);
void put(const char* s);
void putln(const char* s);
void put_dec(uint64_t v);
void put_hex(uint64_t v);
void put_status(mx_status_t s);
const char* cap_type_name(uint32_t type);
const char* boot_type_name(uint32_t type);
int notimpl(const char* what);
bool svc_registry_valid_snapshot(const mochi::svc::RegistryPage* page);
uint16_t svc_registry_entry_count();
bool svc_registry_copy_entry(uint16_t index, mochi::svc::RegistryEntry* out);
bool svc_exists(const char* name);

// --- msh_line.cc ----------------------------------------------------------
uint32_t build_prompt(char* out, uint32_t cap);
bool read_line(char* out, uint32_t cap);

// --- msh_parse.cc ---------------------------------------------------------
EnvVar* find_var(const char* name);
bool set_var(const char* name, const char* value);
bool parse_line(const char* line, ParsedLine* out);
// Split a NAME=VALUE token in place, set the variable, and restore the '='.
bool set_var_from_assignment(char* token);
int execute_simple(int argc, char** argv);
int execute_parsed(ParsedLine* p);

// --- builtins -------------------------------------------------------------
// Core builtins (msh_builtins.cc).
int cmd_help(int argc, char** argv);
int cmd_exit(int argc, char** argv);
int cmd_echo(int argc, char** argv);
int cmd_printf(int argc, char** argv);
int cmd_clear(int argc, char** argv);
int cmd_set(int argc, char** argv);
int cmd_unset(int argc, char** argv);
// Filesystem builtins (msh_fs.cc).
int cmd_pwd(int argc, char** argv);
int cmd_cd(int argc, char** argv);
int cmd_ls(int argc, char** argv);
int cmd_cat(int argc, char** argv);
int cmd_mkdir(int argc, char** argv);
int cmd_rm(int argc, char** argv);
int cmd_touch(int argc, char** argv);
int cmd_sync(int argc, char** argv);
int cmd_fscheck(int argc, char** argv);
// Diagnostics (msh_diag.cc).
int cmd_diag(int argc, char** argv);
int cmd_ipc(int argc, char** argv);
// Introspection builtins (msh_svc.cc).
int cmd_bootinfo(int argc, char** argv);
int cmd_svc(int argc, char** argv);
int cmd_cap(int argc, char** argv);
int cmd_arena(int argc, char** argv);
int cmd_ring(int argc, char** argv);
int cmd_doorbell(int argc, char** argv);
int cmd_vm(int argc, char** argv);
// Power requests (msh_power.cc).
int cmd_shutdown(int argc, char** argv);
int cmd_reboot(int argc, char** argv);
