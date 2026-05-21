// pulse/http_parser.cpp — Toy HTTP/1.1 parser.

#include "pulse/http_request.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace pulse::http {

namespace {

constexpr size_t kMaxHeaderBytes = 8192;     // 431 if exceeded
constexpr size_t kMaxHeaders     = 100;

// Find CRLF starting at `from`. Returns npos if not found.
size_t find_crlf(std::string_view s, size_t from) {
    while (from + 1 < s.size()) {
        if (s[from] == '\r' && s[from + 1] == '\n') return from;
        ++from;
    }
    return std::string_view::npos;
}

// Strip leading/trailing OWS (optional whitespace = SP / HTAB).
std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

} // namespace

ParseResult parse_request(std::string_view input, Request& out) {
    // Cap our scan at kMaxHeaderBytes to avoid pathological inputs DoSing the parser.
    std::string_view scan = input.substr(0, std::min(input.size(), kMaxHeaderBytes + 4));

    // === Request line ===
    size_t line_end = find_crlf(scan, 0);
    if (line_end == std::string_view::npos) {
        // Could be incomplete or could be a tarpit. If we've already buffered
        // more than the cap with no CRLF, it's not coming.
        if (input.size() > kMaxHeaderBytes) return {ParseStatus::kTooLarge, 0};
        return {ParseStatus::kIncomplete, 0};
    }

    std::string_view req_line = scan.substr(0, line_end);

    // Method SP target SP HTTP/1.x
    size_t sp1 = req_line.find(' ');
    if (sp1 == std::string_view::npos) return {ParseStatus::kBadRequest, 0};
    size_t sp2 = req_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return {ParseStatus::kBadRequest, 0};

    out.method = req_line.substr(0, sp1);
    out.target = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = req_line.substr(sp2 + 1);

    if (out.method.empty() || out.target.empty()) {
        return {ParseStatus::kBadRequest, 0};
    }

    // Version check — we accept HTTP/1.0 and HTTP/1.1.
    if (version == "HTTP/1.1") {
        out.http_minor = 1;
        out.keep_alive = true;   // 1.1 default
    } else if (version == "HTTP/1.0") {
        out.http_minor = 0;
        out.keep_alive = false;  // 1.0 default
    } else {
        return {ParseStatus::kBadRequest, 0};
    }

    // Split path/query for routing convenience.
    if (auto q = out.target.find('?'); q != std::string_view::npos) {
        out.path = out.target.substr(0, q);
    } else {
        out.path = out.target;
    }

    // === Headers ===
    size_t pos = line_end + 2;
    out.headers.clear();
    out.headers.reserve(16);

    while (true) {
        if (pos + 1 >= scan.size()) {
            // Not enough data to even check for end-of-headers CRLF.
            if (input.size() > kMaxHeaderBytes) return {ParseStatus::kTooLarge, 0};
            return {ParseStatus::kIncomplete, 0};
        }

        // Blank line → end of headers.
        if (scan[pos] == '\r' && scan[pos + 1] == '\n') {
            pos += 2;
            break;
        }

        size_t eol = find_crlf(scan, pos);
        if (eol == std::string_view::npos) {
            if (input.size() > kMaxHeaderBytes) return {ParseStatus::kTooLarge, 0};
            return {ParseStatus::kIncomplete, 0};
        }

        std::string_view line = scan.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon == std::string_view::npos) return {ParseStatus::kBadRequest, 0};

        std::string_view name  = line.substr(0, colon);
        std::string_view value = trim_ows(line.substr(colon + 1));

        if (name.empty()) return {ParseStatus::kBadRequest, 0};
        // RFC 7230 §3.2.4: no whitespace between field-name and ":".
        if (name.back() == ' ' || name.back() == '\t') return {ParseStatus::kBadRequest, 0};

        if (out.headers.size() >= kMaxHeaders) return {ParseStatus::kTooLarge, 0};
        out.headers.push_back({name, value});

        // Apply semantically meaningful headers.
        if (iequals(name, "Content-Length")) {
            unsigned long long n = 0;
            auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (ec != std::errc()) return {ParseStatus::kBadRequest, 0};
            out.content_length = static_cast<size_t>(n);
            out.has_body = (n > 0);
        } else if (iequals(name, "Connection")) {
            // "close" overrides default; "keep-alive" enables for HTTP/1.0.
            if (iequals(value, "close"))            out.keep_alive = false;
            else if (iequals(value, "keep-alive"))  out.keep_alive = true;
        }
        // NB: we deliberately don't handle Transfer-Encoding: chunked here.
        // Static-file serving doesn't require client-chunked uploads; if we
        // add POST handling later, this is where chunked decoding goes.

        pos = eol + 2;
    }

    return {ParseStatus::kOk, pos};
}

} // namespace pulse::http
