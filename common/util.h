// RouteFlow — time, identifiers, string helpers, leveled logging.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rf {

// --- time -------------------------------------------------------------------

int64_t now_ms();            // wall clock, epoch ms (timestamps in the trace)
int64_t mono_ms();           // monotonic ms (every duration we measure)

// ISO-8601 UTC with milliseconds: 2026-09-03T14:21:07.412Z
std::string iso8601(int64_t epoch_ms);

// Inverse of iso8601. Returns 0 if the string is not a timestamp we wrote.
int64_t parse_iso8601(const std::string& s);

// --- identifiers ------------------------------------------------------------

// ULID: lexicographically sortable, monotonic within a process. Job ids must
// sort in dispatch order so a trace file reads chronologically.
std::string ulid();

// --- strings ----------------------------------------------------------------

std::string trim(const std::string& s);
std::string lower(std::string s);
bool iequals(const std::string& a, const std::string& b);
bool starts_with(const std::string& s, const std::string& prefix);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, const char* sep);

// Token-count estimate without a tokenizer dependency (ARCHITECTURE §12, D15).
// Byte-class heuristic; the trace records this alongside the engine-reported
// actual so the error is measured rather than argued about.
uint32_t estimate_tokens(const std::string& utf8);

// --- files ------------------------------------------------------------------

bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& data);

// --- logging ----------------------------------------------------------------

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

void log_set_level(LogLevel level);
bool log_set_level(const std::string& name);  // "info", "debug", ... ; false if unknown
LogLevel log_level();
void log_write(LogLevel level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define RF_LOG(lvl, ...)                                   \
    do {                                                   \
        if (static_cast<int>(::rf::log_level()) <=         \
            static_cast<int>(::rf::LogLevel::lvl))         \
            ::rf::log_write(::rf::LogLevel::lvl, __VA_ARGS__); \
    } while (0)

#define RF_TRACE(...) RF_LOG(Trace, __VA_ARGS__)
#define RF_DEBUG(...) RF_LOG(Debug, __VA_ARGS__)
#define RF_INFO(...)  RF_LOG(Info,  __VA_ARGS__)
#define RF_WARN(...)  RF_LOG(Warn,  __VA_ARGS__)
#define RF_ERROR(...) RF_LOG(Error, __VA_ARGS__)

}  // namespace rf
