#pragma once
// anko/log.h — kernel logging facade.
//
// Normal logs are non-blocking.  Early boot and fatal logs use the UART
// fallback; once the console service is wired, logs are forwarded through
// per-CPU SPSC rings and one shared doorbell.

#include <cstdarg>
#include "anko/types.h"

struct Doorbell;
struct Ring;

namespace anko::log {

enum class Level : u8 {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

void early_init();
void attach_console(Ring* const* rings, Doorbell* doorbell, u32 ring_count);

void write(Level level, const char* component, const char* msg);
void writef(Level level, const char* component, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void vwritef(Level level, const char* component, const char* fmt, va_list ap);

void raw(const char* msg);
void rawf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void vrawf(const char* fmt, va_list ap);

// Format into a caller-supplied buffer (NUL-terminated, always bounded). Used by
// the panic screen to render a formatted detail line without going through the
// log ring.
void vformat_to(char* out, usize cap, const char* fmt, va_list ap);

u64 dropped_count();
u64 truncated_count();

} // namespace anko::log

#define ANKO_LOG_TRACE(component, msg) \
    ::anko::log::write(::anko::log::Level::Trace, component, msg)
#define ANKO_LOG_DEBUG(component, msg) \
    ::anko::log::write(::anko::log::Level::Debug, component, msg)
#define ANKO_LOG_INFO(component, msg) \
    ::anko::log::write(::anko::log::Level::Info, component, msg)
#define ANKO_LOG_WARN(component, msg) \
    ::anko::log::write(::anko::log::Level::Warn, component, msg)
#define ANKO_LOG_ERROR(component, msg) \
    ::anko::log::write(::anko::log::Level::Error, component, msg)
#define ANKO_LOG_FATAL(component, msg) \
    ::anko::log::write(::anko::log::Level::Fatal, component, msg)

#define ANKO_LOGF_TRACE(component, fmt, ...) \
    ::anko::log::writef(::anko::log::Level::Trace, component, fmt, ##__VA_ARGS__)
#define ANKO_LOGF_DEBUG(component, fmt, ...) \
    ::anko::log::writef(::anko::log::Level::Debug, component, fmt, ##__VA_ARGS__)
#define ANKO_LOGF_INFO(component, fmt, ...) \
    ::anko::log::writef(::anko::log::Level::Info, component, fmt, ##__VA_ARGS__)
#define ANKO_LOGF_WARN(component, fmt, ...) \
    ::anko::log::writef(::anko::log::Level::Warn, component, fmt, ##__VA_ARGS__)
#define ANKO_LOGF_ERROR(component, fmt, ...) \
    ::anko::log::writef(::anko::log::Level::Error, component, fmt, ##__VA_ARGS__)
#define ANKO_LOGF_FATAL(component, fmt, ...) \
    ::anko::log::writef(::anko::log::Level::Fatal, component, fmt, ##__VA_ARGS__)

#define ANKO_LOG_RAW(msg) ::anko::log::raw(msg)
#define ANKO_LOGF_RAW(fmt, ...) ::anko::log::rawf(fmt, ##__VA_ARGS__)
