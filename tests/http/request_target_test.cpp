#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "sparenode/http/detail/buffered_request.hpp"
#include "sparenode/http/http_request_parser.hpp"
#include "support/optional.hpp"
#include <string>
#include <vector>

namespace
{
/// @brief Borrows request fixture storage as bytes.
std::span<const std::byte> bytes(const std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}
} // namespace

TEST_CASE("Absolute targets normalize path and query without decoding", "[http][target]")
{
    const auto fixture = GENERATE(
        std::pair{"http://local/a%2Fb?x=%2F", "/a%2Fb?x=%2F"}, std::pair{"HtTp://LOCAL:80", "/"},
        std::pair{"http://local?x=one?two", "/?x=one?two"}, std::pair{"http://local?", "/?"},
        std::pair{"http://[::1]:8080/path", "/path"},
        std::pair{"http://[0:0:0:0:0:ffff:192.0.2.1]/", "/"},
        std::pair{"http://127.0.0.1:80//a/../b", "//a/../b"});
    const std::string source =
        std::string("GET ") + fixture.first + " HTTP/1.1\r\nHost: local\r\n\r\n";
    auto result = sparenode::http::parse_http_request(bytes(source));
    REQUIRE(result.has_value());
    REQUIRE(result->is_complete());
    CHECK(result->consumed_bytes() == source.size());
    auto copy = sparenode::test::require_optional(result->request());
    result = sparenode::http::HttpRequestParseResult::incomplete();
    CHECK(copy.target() == fixture.second);
}

TEST_CASE("Absolute authority overrides every downstream Host view", "[http][target][security]")
{
    const std::string host = GENERATE("different.example", "local:81", "[::1]:443", "LOCAL:80");
    const std::string source = "GET http://local:80/path HTTP/1.1\r\nhOsT: " + host + "\r\n\r\n";
    const auto result = sparenode::http::parse_http_request(bytes(source));
    REQUIRE(result.has_value());
    const auto &request = sparenode::test::require_optional(result->request());
    CHECK(request.header("host") == "local:80");
    const auto hosts = request.headers("HOST");
    REQUIRE(hosts.size() == 1);
    CHECK(hosts.front() == "local:80");
    REQUIRE(request.fields().size() == 1);
    CHECK(request.fields().front().value == "local:80");
    CHECK(source.contains(host));
}

TEST_CASE("Invalid absolute authority and target syntax fail closed", "[http][target][security]")
{
    const std::string target = GENERATE(
        "http:///path", "http://", "http://user@local/path", "http://local:65536/",
        "http://local:/", "http://local:abc/", "http://[::1/path", "http://::1/path",
        "http://[v1.a]/", "http://[fe80::1%25eth0]/", "http://a%2eb/", "http://local#fragment",
        "http://local/path#fragment", "http://local/%GG", "http://local\\other/path",
        "https://local/", "ftp://local/", "local:80", "http:/local/", "*", "http://local?x=[y]");
    const std::string source = "GET " + target + " HTTP/1.1\r\nHost: local\r\n\r\n";
    const auto result = sparenode::http::parse_http_request(bytes(source));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code ==
          sparenode::http::HttpRequestParseErrorCode::invalid_request_target);
    CHECK(result.error().byte_offset == 4);
}

TEST_CASE("Absolute form still requires a single syntactically valid Host",
          "[http][target][security]")
{
    const std::string fields =
        GENERATE("", "Host: \r\n", "Host: local\r\nHost: other\r\n", "Host: bad host\r\n");
    const std::string source = "GET http://local/ HTTP/1.1\r\n" + fields + "\r\n";
    CHECK_FALSE(sparenode::http::parse_http_request(bytes(source)).has_value());
}

TEST_CASE("HTTP version policy accepts higher minors and separates syntax from support",
          "[http][version]")
{
    using Code = sparenode::http::HttpRequestParseErrorCode;
    SECTION("supported minors")
    {
        const std::string minor = GENERATE("1", "2", "3", "4", "5", "6", "7", "8", "9");
        const std::string source = "GET / HTTP/1." + minor + "\r\nHost: local\r\n\r\n";
        const auto result = sparenode::http::parse_http_request(bytes(source));
        REQUIRE(result.has_value());
        CHECK(result->is_complete());
    }
    SECTION("unsupported well-formed versions")
    {
        const std::string version = GENERATE("HTTP/1.0", "HTTP/0.9", "HTTP/2.0", "HTTP/9.9");
        const std::string source = "GET / " + version + "\r\nHost: local\r\n\r\n";
        const auto result = sparenode::http::parse_http_request(bytes(source));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::unsupported_http_version);
    }
    SECTION("malformed versions")
    {
        const std::string version = GENERATE("HTTP/1.10", "HTTP/01.1", "http/1.1", "HTTP/1",
                                             "HTTP/1.x", "HTTP/+1.1", "HTTP/1.1junk", "");
        const std::string source = "GET / " + version + "\r\nHost: local\r\n\r\n";
        const auto result = sparenode::http::parse_http_request(bytes(source));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Code::malformed_http_version);
    }
}

TEST_CASE("Absolute wire length is bounded before normalization", "[http][target][limits]")
{
    const std::string line = "GET http://local/path HTTP/1.1";
    const std::string source = line + "\r\nHost: local\r\n\r\n";
    sparenode::http::HttpRequestParserLimits limits{.max_request_line_bytes = line.size()};
    REQUIRE(sparenode::http::parse_http_request(bytes(source), limits).has_value());
    --limits.max_request_line_bytes;
    const auto rejected = sparenode::http::parse_http_request(bytes(source), limits);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          sparenode::http::HttpRequestParseErrorCode::request_line_too_large);
}

TEST_CASE("Normalized query target survives incremental chunked ingestion",
          "[http][target][chunked]")
{
    const std::string source = "POST http://local?x=1 HTTP/1.9\r\nHost: "
                               "other\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n";
    sparenode::http::detail::BufferedRequest request({});
    for (std::size_t index = 0; index < source.size(); ++index)
    {
        REQUIRE(request.feed(bytes(source).subspan(index, 1)).has_value());
    }
    REQUIRE(request.complete());
    const auto parsed = request.request();
    CHECK(parsed.target() == "/?x=1");
    CHECK(parsed.header("Host") == "local");
    REQUIRE(parsed.body().size() == 1);
    CHECK(parsed.body().front() == std::byte{'x'});
}
