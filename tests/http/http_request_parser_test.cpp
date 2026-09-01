#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/http/http_request_parser.hpp"

namespace
{

/// @brief Borrows string bytes for the parser without copying them.
[[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view input) noexcept
{
    return {reinterpret_cast<const std::byte *>(input.data()), input.size()};
}

/// @brief Checks one source against an expected terminal parse failure.
void require_error(const std::string_view source,
                   const sparenode::http::HttpRequestParseErrorCode expected)
{
    const auto result = sparenode::http::parse_http_request(as_bytes(source));
    REQUIRE(!result.has_value());
    CHECK(result.error().code == expected);
}

} // namespace

TEST_CASE("HTTP request parser returns a complete borrowed request and exact boundary",
          "[http][request][parser]")
{
    const std::string source = "POST /files/report%20copy.txt?next=/a?b&replace=true HTTP/1.1\r\n"
                               "Host:\t node.local \t\r\n"
                               "Content-Type: text/plain\r\n"
                               "X-Tag: first\r\n"
                               "x-tag: second\r\n"
                               "Content-Length: 5\r\n\r\n"
                               "helloNEXT";

    const auto result = sparenode::http::parse_http_request(as_bytes(source));

    REQUIRE(result.has_value());
    REQUIRE(result->complete);
    CHECK(result->request.method() == sparenode::http::HttpMethod::post);
    CHECK(result->request.target() == "/files/report%20copy.txt?next=/a?b&replace=true");
    CHECK(result->request.header("host") == "node.local");
    CHECK(result->request.header("CONTENT-TYPE") == "text/plain");
    CHECK(result->request.header("missing").empty());
    const auto tag_values = result->request.headers("X-TAG");
    REQUIRE(tag_values.size() == 2);
    CHECK(tag_values[0] == "first");
    CHECK(tag_values[1] == "second");
    const std::span body_bytes = result->request.body();
    const std::string_view body(reinterpret_cast<const char *>(body_bytes.data()),
                                body_bytes.size());
    CHECK(body == "hello");
    CHECK(source.substr(result->consumed_bytes) == "NEXT");
}

TEST_CASE("HTTP request parser accepts the documented method subset", "[http][request][parser]")
{
    const std::vector<std::pair<std::string_view, sparenode::http::HttpMethod>> cases{
        {"GET", sparenode::http::HttpMethod::get},
        {"HEAD", sparenode::http::HttpMethod::head},
        {"POST", sparenode::http::HttpMethod::post},
        {"PUT", sparenode::http::HttpMethod::put},
        {"DELETE", sparenode::http::HttpMethod::delete_method},
        {"OPTIONS", sparenode::http::HttpMethod::options}};

    for (const auto &[text, expected] : cases)
    {
        DYNAMIC_SECTION(text)
        {
            const std::string source = std::string(text) + " / HTTP/1.1\r\nHost: localhost\r\n\r\n";
            const auto result = sparenode::http::parse_http_request(as_bytes(source));
            REQUIRE(result.has_value());
            REQUIRE(result->complete);
            CHECK(result->request.method() == expected);
            CHECK(sparenode::http::to_string(expected) == text);
        }
    }
}

TEST_CASE("HTTP request parser distinguishes incomplete input from malformed input",
          "[http][request][parser]")
{
    const std::array incomplete_sources{
        std::string_view{"GET / HTTP/1.1"}, std::string_view{"GET / HTTP/1.1\r\nHost: local"},
        std::string_view{"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\nab"}};

    for (const std::string_view source : incomplete_sources)
    {
        const auto result = sparenode::http::parse_http_request(as_bytes(source));
        REQUIRE(result.has_value());
        CHECK(!result->complete);
        CHECK(result->consumed_bytes == 0);
    }
}

TEST_CASE("HTTP request parser rejects malformed and ambiguous protocol syntax",
          "[http][request][parser][errors]")
{
    using Code = sparenode::http::HttpRequestParseErrorCode;
    const std::vector<std::pair<std::string, Code>> cases{
        {"GET / HTTP/1.1\nHost: local\n\n", Code::invalid_line_ending},
        {"GET /\r\nHost: local\r\n\r\n", Code::malformed_request_line},
        {"G@T / HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_method},
        {"PATCH / HTTP/1.1\r\nHost: local\r\n\r\n", Code::unsupported_method},
        {"GET http://local/ HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET /doc#part HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET /% HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET /%A HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET /%GG HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET /a\\b HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET /?value=[x] HTTP/1.1\r\nHost: local\r\n\r\n", Code::invalid_request_target},
        {"GET / HTTP/1.0\r\nHost: local\r\n\r\n", Code::unsupported_http_version},
        {"GET / HTTP/1.1\r\nBroken\r\n\r\n", Code::malformed_header},
        {"GET / HTTP/1.1\r\n Host: local\r\n\r\n", Code::folded_header},
        {"GET / HTTP/1.1\r\nX-Test: yes\r\n\r\n", Code::missing_host},
        {"GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n", Code::duplicate_host},
        {"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: four\r\n\r\n",
         Code::invalid_content_length},
        {"POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na",
         Code::duplicate_content_length},
        {"POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n",
         Code::unsupported_transfer_encoding}};

    for (const auto &[source, expected] : cases)
    {
        DYNAMIC_SECTION(source)
        {
            require_error(source, expected);
        }
    }
}

TEST_CASE("HTTP request parser enforces every configured size boundary",
          "[http][request][parser][limits]")
{
    using Code = sparenode::http::HttpRequestParseErrorCode;

    SECTION("request line")
    {
        sparenode::http::HttpRequestParserLimits limits;
        limits.max_request_line_bytes = 8;
        const auto result = sparenode::http::parse_http_request(
            as_bytes("GET /long HTTP/1.1\r\nHost: local\r\n\r\n"), limits);
        REQUIRE(!result.has_value());
        CHECK(result.error().code == Code::request_line_too_large);
    }
    SECTION("header bytes")
    {
        sparenode::http::HttpRequestParserLimits limits;
        limits.max_header_bytes = 8;
        const auto result = sparenode::http::parse_http_request(
            as_bytes("GET / HTTP/1.1\r\nHost: local\r\n\r\n"), limits);
        REQUIRE(!result.has_value());
        CHECK(result.error().code == Code::headers_too_large);
    }
    SECTION("header count")
    {
        sparenode::http::HttpRequestParserLimits limits;
        limits.max_header_count = 1;
        const auto result = sparenode::http::parse_http_request(
            as_bytes("GET / HTTP/1.1\r\nHost: local\r\nX-Test: yes\r\n\r\n"), limits);
        REQUIRE(!result.has_value());
        CHECK(result.error().code == Code::too_many_headers);
    }
    SECTION("body bytes")
    {
        sparenode::http::HttpRequestParserLimits limits;
        limits.max_body_bytes = 3;
        const auto result = sparenode::http::parse_http_request(
            as_bytes("POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\ndata"), limits);
        REQUIRE(!result.has_value());
        CHECK(result.error().code == Code::body_too_large);
    }
}

TEST_CASE("HTTP request parser rejects overflowing Content-Length",
          "[http][request][parser][limits]")
{
    require_error("POST / HTTP/1.1\r\nHost: local\r\n"
                  "Content-Length: 184467440737095516160\r\n\r\n",
                  sparenode::http::HttpRequestParseErrorCode::invalid_content_length);
}

TEST_CASE("HTTP request parser preserves a source offset for protocol failures",
          "[http][request][parser][errors]")
{
    const std::string source = "GET / HTTP/1.1\r\nHost: local\r\nBroken\r\n\r\n";
    const auto result = sparenode::http::parse_http_request(as_bytes(source));

    REQUIRE(!result.has_value());
    CHECK(result.error().code == sparenode::http::HttpRequestParseErrorCode::malformed_header);
    CHECK(result.error().byte_offset == source.find("Broken"));
}

TEST_CASE("HTTP request parser error descriptions are stable", "[http][request][parser][errors]")
{
    using Code = sparenode::http::HttpRequestParseErrorCode;
    const std::array cases{
        std::pair{Code::request_line_too_large,
                  std::string_view{"HTTP request line exceeds its configured limit"}},
        std::pair{Code::headers_too_large,
                  std::string_view{"HTTP headers exceed their configured limit"}},
        std::pair{Code::too_many_headers,
                  std::string_view{"HTTP request contains too many headers"}},
        std::pair{Code::body_too_large,
                  std::string_view{"HTTP request body exceeds its configured limit"}},
        std::pair{Code::invalid_line_ending,
                  std::string_view{"HTTP protocol lines must end with CRLF"}},
        std::pair{Code::malformed_request_line, std::string_view{"HTTP request line is malformed"}},
        std::pair{Code::invalid_method, std::string_view{"HTTP method is not a valid token"}},
        std::pair{Code::unsupported_method, std::string_view{"HTTP method is not supported"}},
        std::pair{Code::invalid_request_target, std::string_view{"HTTP request target is invalid"}},
        std::pair{Code::unsupported_http_version,
                  std::string_view{"HTTP version is not supported"}},
        std::pair{Code::malformed_header, std::string_view{"HTTP header is malformed"}},
        std::pair{Code::folded_header, std::string_view{"folded HTTP headers are not supported"}},
        std::pair{Code::missing_host, std::string_view{"HTTP/1.1 Host header is missing"}},
        std::pair{Code::duplicate_host, std::string_view{"HTTP/1.1 Host header is repeated"}},
        std::pair{Code::invalid_content_length, std::string_view{"HTTP Content-Length is invalid"}},
        std::pair{Code::duplicate_content_length,
                  std::string_view{"HTTP Content-Length is repeated"}},
        std::pair{Code::unsupported_transfer_encoding,
                  std::string_view{"HTTP Transfer-Encoding is not supported"}}};

    for (const auto &[code, expected] : cases)
    {
        CHECK(std::string_view{sparenode::http::to_string(code)} == expected);
    }
}
