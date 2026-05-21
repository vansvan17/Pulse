#include "pulse/http_request.hpp"
#include "test_harness.hpp"

#include <string>

using namespace pulse;
using namespace pulse::test;

int main() {
    {
        start("simple_get_http11");
        http::Request req;
        std::string input = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kOk));
        EXPECT_EQ(pr.consumed, input.size());
        EXPECT_STREQ(std::string(req.method), "GET");
        EXPECT_STREQ(std::string(req.path), "/index.html");
        EXPECT_TRUE(req.keep_alive);
        finish();
    }
    {
        start("http10_default_close");
        http::Request req;
        std::string input = "GET / HTTP/1.0\r\nHost: x\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kOk));
        EXPECT_TRUE(!req.keep_alive);
        finish();
    }
    {
        start("http10_keep_alive_explicit");
        http::Request req;
        std::string input = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kOk));
        EXPECT_TRUE(req.keep_alive);
        finish();
    }
    {
        start("http11_connection_close");
        http::Request req;
        std::string input = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kOk));
        EXPECT_TRUE(!req.keep_alive);
        finish();
    }
    {
        start("query_string_stripped");
        http::Request req;
        std::string input = "GET /foo?a=1&b=2 HTTP/1.1\r\n\r\n";
        http::parse_request(input, req);
        EXPECT_STREQ(std::string(req.path), "/foo");
        EXPECT_STREQ(std::string(req.target), "/foo?a=1&b=2");
        finish();
    }
    {
        start("incomplete_returns_incomplete");
        http::Request req;
        std::string input = "GET / HTTP/1.1\r\nHost: x";   // no \r\n\r\n
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kIncomplete));
        finish();
    }
    {
        start("malformed_request_line");
        http::Request req;
        std::string input = "BROKEN\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kBadRequest));
        finish();
    }
    {
        start("case_insensitive_header_lookup");
        http::Request req;
        std::string input = "GET / HTTP/1.1\r\nCONTENT-LENGTH: 5\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kOk));
        EXPECT_EQ(req.content_length, 5u);
        EXPECT_STREQ(std::string(req.find_header("content-length")), "5");
        finish();
    }
    {
        start("rejects_oversize_headers");
        http::Request req;
        std::string input = "GET / HTTP/1.1\r\nX-Huge: ";
        input += std::string(10'000, 'a');
        input += "\r\n\r\n";
        auto pr = http::parse_request(input, req);
        EXPECT_EQ(static_cast<int>(pr.status), static_cast<int>(http::ParseStatus::kTooLarge));
        finish();
    }
    return summary();
}
