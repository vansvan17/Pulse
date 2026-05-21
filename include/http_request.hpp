// pulse/http_request.hpp — Parsed HTTP/1.1 request representation.
//
// Memory model:
//   Headers are stored as (string_view, string_view) pairs pointing INTO the
//   connection's read buffer. This is zero-copy parsing but creates a
//   lifetime contract: the request is only valid while the underlying buffer
//   region is unconsumed. The connection state machine respects this by not
//   consuming the request bytes from the buffer until the response is queued.
//
//   If we wanted to keep a request beyond the lifetime of its buffer region
//   (e.g., to dispatch to a worker thread), we'd need to copy headers to
//   owned strings. We don't, today.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::http {

struct Header {
    std::string_view name;
    std::string_view value;
};

struct Request {
    std::string_view method;
    std::string_view target;     // raw request target, e.g. "/index.html?x=1"
    std::string_view path;       // target with query stripped
    int http_minor = 1;          // 1 = HTTP/1.1, 0 = HTTP/1.0

    std::vector<Header> headers;
    size_t content_length = 0;
    bool has_body = false;

    // Final keep-alive decision after applying RFC 7230 §6.3 rules:
    //   - HTTP/1.1 default: keep-alive
    //   - HTTP/1.0 default: close, unless "Connection: keep-alive"
    //   - Explicit "Connection: close" always wins
    bool keep_alive = false;

    std::string_view find_header(std::string_view name) const {
        for (const auto& h : headers) {
            if (h.name.size() != name.size()) continue;
            // Case-insensitive compare per RFC 7230 §3.2.
            if (std::equal(h.name.begin(), h.name.end(), name.begin(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          })) {
                return h.value;
            }
        }
        return {};
    }
};

// Parser result. We separate "need more data" from "malformed" because they
// drive very different state transitions in the connection FSM.
enum class ParseStatus {
    kIncomplete,   // No complete request yet; read more bytes.
    kOk,           // Complete request parsed; `consumed` bytes are now safe to discard.
    kBadRequest,   // Malformed; respond 400 and close.
    kTooLarge,     // Headers exceed limit; respond 431 and close.
};

struct ParseResult {
    ParseStatus status;
    size_t consumed = 0;   // Bytes of input consumed (request line + headers + CRLF).
};

// Parse a single HTTP/1.1 request from `input`. This is a toy parser:
//   - Recognizes request line + headers, validates basic structure
//   - Computes content_length and keep_alive
//   - Does NOT consume the body. The connection FSM reads `content_length`
//     bytes of body separately after parsing.
//
// Replace with picohttpparser once we want robustness against the long tail
// of HTTP weirdness (folded headers, OWS handling, chunked encoding).
ParseResult parse_request(std::string_view input, Request& out);

} // namespace pulse::http
