// pulse/http_response.hpp — Build HTTP/1.1 responses into a Buffer.

#pragma once

#include <cstdio>
#include <ctime>
#include <string_view>

#include "buffer.hpp"

namespace pulse::http {

inline const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

// IMF-fixdate per RFC 7231 §7.1.1.1, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
inline void write_date_header(Buffer& out) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    size_t n = std::strftime(buf, sizeof(buf), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
    out.append(buf, n);
}

// Write a complete response header section (no body).
// `body_len` should be the exact body size; we always include Content-Length
// to enable keep-alive (otherwise client has to wait for connection close).
inline void write_headers(Buffer& out,
                          int status,
                          std::string_view content_type,
                          size_t body_len,
                          bool keep_alive,
                          int http_minor = 1) {
    char line[256];
    int n = std::snprintf(line, sizeof(line), "HTTP/1.%d %d %s\r\n",
                          http_minor, status, status_text(status));
    out.append(line, static_cast<size_t>(n));

    write_date_header(out);
    out.append("Server: pulse/0.1\r\n");

    n = std::snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body_len);
    out.append(line, static_cast<size_t>(n));

    if (!content_type.empty()) {
        out.append("Content-Type: ");
        out.append(content_type);
        out.append("\r\n");
    }

    // Connection header. HTTP/1.1 defaults to keep-alive so we only need to
    // be explicit when we differ from the default — but being explicit is
    // friendlier for proxies and debuggers.
    out.append(keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");

    out.append("\r\n");
}

// Write a complete tiny response (headers + body) inline. Used for errors and
// for very small static responses where sendfile is overkill.
inline void write_simple(Buffer& out, int status, std::string_view body,
                         bool keep_alive, int http_minor = 1,
                         std::string_view content_type = "text/plain; charset=utf-8") {
    write_headers(out, status, content_type, body.size(), keep_alive, http_minor);
    out.append(body);
}

} // namespace pulse::http
