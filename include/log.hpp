// pulse/log.hpp — Minimal structured (JSON-lines) logger.
//
// Why structured logging:
//   "client connected" is useless at scale. With JSON-lines, we can grep,
//   jq, or pipe into a log shipper. Each line is a complete event with
//   timestamp + level + message + arbitrary key/value fields.
//
// Why this is not std::cout << ...:
//   std::cout is not atomic across threads — interleaved output is the
//   norm. We buffer a single line and write(2) it atomically. write(2) on
//   a regular file or pipe is atomic up to PIPE_BUF (4096 bytes on Linux),
//   which is plenty for one log line.
//
// Tradeoff:
//   This is a synchronous logger writing to stderr. For very high RPS we'd
//   want an async ring-buffer with a dedicated drain thread. That's a known
//   next step but premature here — we want correctness first, and being able
//   to see "what did the server do during that benchmark" cheaply.

#pragma once

#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace pulse::log {

enum class Level { kDebug, kInfo, kWarn, kError };

inline const char* level_str(Level l) {
    switch (l) {
        case Level::kDebug: return "debug";
        case Level::kInfo:  return "info";
        case Level::kWarn:  return "warn";
        case Level::kError: return "error";
    }
    return "unknown";
}

inline Level g_min_level = Level::kInfo;

inline void set_level(Level l) { g_min_level = l; }

// Escape a string for embedding in a JSON value. Minimal — enough for our
// own messages and known-safe values. We don't ship user-controlled headers
// through here without sanitization.
inline void json_escape(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

class LineBuilder {
public:
    LineBuilder(Level l, std::string_view event) : level_(l) {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto secs = duration_cast<seconds>(now.time_since_epoch()).count();
        auto us   = duration_cast<microseconds>(now.time_since_epoch()).count() % 1'000'000;

        buf_.reserve(256);
        buf_ += "{\"ts\":\"";
        char tsbuf[64];
        std::snprintf(tsbuf, sizeof(tsbuf), "%lld.%06lld",
                      static_cast<long long>(secs), static_cast<long long>(us));
        buf_ += tsbuf;
        buf_ += "\",\"level\":\"";
        buf_ += level_str(l);
        buf_ += "\",\"event\":\"";
        json_escape(buf_, event);
        buf_ += '"';
    }

    LineBuilder& kv(std::string_view key, std::string_view value) {
        buf_ += ",\"";
        json_escape(buf_, key);
        buf_ += "\":\"";
        json_escape(buf_, value);
        buf_ += '"';
        return *this;
    }

    LineBuilder& kv(std::string_view key, long long value) {
        buf_ += ",\"";
        json_escape(buf_, key);
        buf_ += "\":";
        char nbuf[32];
        std::snprintf(nbuf, sizeof(nbuf), "%lld", value);
        buf_ += nbuf;
        return *this;
    }

    LineBuilder& kv(std::string_view key, int value)    { return kv(key, static_cast<long long>(value)); }
    LineBuilder& kv(std::string_view key, size_t value) { return kv(key, static_cast<long long>(value)); }

    ~LineBuilder() {
        if (level_ < g_min_level) return;
        buf_ += "}\n";
        // Atomic write to stderr fd. Single syscall, no interleaving up to PIPE_BUF.
        // Intentionally best-effort: a failed log write is not worth crashing for.
        ssize_t r = ::write(2, buf_.data(), buf_.size());
        (void)r;
    }

private:
    Level level_;
    std::string buf_;
};

inline LineBuilder debug(std::string_view event) { return LineBuilder(Level::kDebug, event); }
inline LineBuilder info (std::string_view event) { return LineBuilder(Level::kInfo,  event); }
inline LineBuilder warn (std::string_view event) { return LineBuilder(Level::kWarn,  event); }
inline LineBuilder error(std::string_view event) { return LineBuilder(Level::kError, event); }

} // namespace pulse::log
