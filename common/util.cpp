#include "util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

namespace rf {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_log_mutex;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t mono_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string iso8601(int64_t epoch_ms) {
    const std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
    const int millis = static_cast<int>(epoch_ms % 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return buf;
}

// Days since 1970-01-01 from a civil date. Howard Hinnant's algorithm; avoids
// timegm(), which is not portable.
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t parse_iso8601(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, ms = 0;
    // "2026-09-03T14:21:07.412Z"; the fractional part is optional.
    const int n = std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d",
                              &y, &mo, &d, &h, &mi, &sec, &ms);
    if (n < 6) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    const int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                         static_cast<unsigned>(d));
    return ((days * 24 + h) * 60 + mi) * 60000LL + sec * 1000LL + (n >= 7 ? ms : 0);
}

std::string ulid() {
    // Crockford base32, 10 chars of ms timestamp + 16 chars of randomness.
    // Monotonic within a process: if two ULIDs land in the same millisecond the
    // random half is incremented rather than redrawn, so dispatch order is
    // preserved in the trace file.
    static const char kEnc[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    static std::mutex mu;
    static int64_t last_ms = -1;
    static uint8_t rnd[10];

    std::lock_guard<std::mutex> lock(mu);
    const int64_t ts = now_ms();
    if (ts == last_ms) {
        for (int i = 9; i >= 0; --i)
            if (++rnd[i] != 0) break;  // carry
    } else {
        static std::random_device rd;
        static std::mt19937_64 gen(rd() ^ static_cast<uint64_t>(now_ms()));
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : rnd) b = static_cast<uint8_t>(dist(gen));
        last_ms = ts;
    }

    std::string out(26, '0');
    int64_t t = ts;
    for (int i = 9; i >= 0; --i) { out[i] = kEnc[t & 31]; t >>= 5; }

    // 10 random bytes -> 16 base32 chars, MSB first.
    uint32_t acc = 0;
    int bits = 0, pos = 10;
    for (int i = 0; i < 10 && pos < 26; ++i) {
        acc = (acc << 8) | rnd[i];
        bits += 8;
        while (bits >= 5 && pos < 26) {
            bits -= 5;
            out[pos++] = kEnc[(acc >> bits) & 31];
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

uint32_t estimate_tokens(const std::string& utf8) {
    // Three byte classes with different tokens-per-byte ratios. Deliberately
    // crude: the point is a cheap estimate whose error the trace measures.
    size_t ascii = 0;    // Latin text and code   ~4.0 bytes/token
    size_t twobyte = 0;  // Latin-ext, Cyrillic, Greek, Turkish diacritics
    size_t wide = 0;     // CJK and above         ~1.5 bytes/token (dense)
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) { ++ascii; i += 1; }
        else if ((c & 0xE0) == 0xC0) { ++twobyte; i += 2; }
        else if ((c & 0xF0) == 0xE0) { ++wide; i += 3; }
        else if ((c & 0xF8) == 0xF0) { ++wide; i += 4; }
        else { ++ascii; i += 1; }  // invalid lead byte; count it, do not stall
    }
    // twobyte codepoints cost ~1 token per 2 chars; wide ~1 per char.
    const double est = static_cast<double>(ascii) / 4.0
                     + static_cast<double>(twobyte) / 2.0
                     + static_cast<double>(wide);
    return static_cast<uint32_t>(est < 1.0 && !utf8.empty() ? 1.0 : est);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

void log_set_level(LogLevel level) { g_level.store(static_cast<int>(level)); }

bool log_set_level(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "trace") { log_set_level(LogLevel::Trace); return true; }
    if (n == "debug") { log_set_level(LogLevel::Debug); return true; }
    if (n == "info")  { log_set_level(LogLevel::Info);  return true; }
    if (n == "warn")  { log_set_level(LogLevel::Warn);  return true; }
    if (n == "error") { log_set_level(LogLevel::Error); return true; }
    return false;
}

LogLevel log_level() { return static_cast<LogLevel>(g_level.load()); }

void log_write(LogLevel level, const char* fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    const std::string ts = iso8601(now_ms());
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stderr, "%s %s %s\n", ts.c_str(), level_name(level), body);
    std::fflush(stderr);
}

}  // namespace rf
