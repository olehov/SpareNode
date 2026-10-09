#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "sparenode/http/request/http_request_parser.hpp"
#include "sparenode/http/session/http_transport_policy.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Borrows request fixture bytes without copying them.
[[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view input) noexcept
{
    return {reinterpret_cast<const std::byte *>(input.data()), input.size()};
}

/// @brief Parses one complete request whose source remains owned by the caller.
[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto result = sparenode::http::parse_http_request(as_bytes(source));
    REQUIRE(result.has_value());
    REQUIRE(result->is_complete());
    return sparenode::test::require_optional(result->request());
}

} // namespace

TEST_CASE("HTTP transport modes use stable configuration spellings", "[http][transport]")
{
    using sparenode::http::HttpTransportMode;

    CHECK(sparenode::http::parse_http_transport_mode("loopback_http") ==
          HttpTransportMode::loopback_http);
    CHECK(sparenode::http::parse_http_transport_mode("trusted_lan_http") ==
          HttpTransportMode::trusted_lan_http);
    CHECK(sparenode::http::parse_http_transport_mode("trusted_proxy_https") ==
          HttpTransportMode::trusted_proxy_https);
    CHECK_FALSE(sparenode::http::parse_http_transport_mode("https"));
    CHECK_FALSE(sparenode::http::parse_http_transport_mode("TRUSTED_PROXY_HTTPS"));
}

TEST_CASE("Direct HTTP modes derive identity only from the socket peer", "[http][transport]")
{
    constexpr std::string_view source =
        "GET / HTTP/1.1\r\nHost: local\r\nX-Forwarded-Proto: https\r\n"
        "X-Forwarded-For: 203.0.113.9\r\n\r\n";
    const auto request = parse_request(source);

    SECTION("loopback mode accepts the complete IPv4 and IPv6 loopback ranges")
    {
        const sparenode::http::HttpTransportPolicy policy;
        CHECK(sparenode::http::http_transport_allows_peer(policy, {"127.9.8.7", 1234}));
        CHECK(sparenode::http::http_transport_allows_peer(policy, {"::1", 1234}));
        CHECK_FALSE(sparenode::http::http_transport_allows_peer(policy, {"192.0.2.1", 1234}));

        const auto context =
            sparenode::http::resolve_http_transport(policy, {"127.0.0.1", 1234}, request);
        REQUIRE(context.has_value());
        CHECK(context->peer_address == "127.0.0.1");
        CHECK(context->client_address == "127.0.0.1");
        CHECK_FALSE(context->secure);
        CHECK_FALSE(context->trusted_proxy);
    }

    SECTION("explicit LAN mode accepts numeric remote peers and ignores forwarding headers")
    {
        const sparenode::http::HttpTransportPolicy policy{
            sparenode::http::HttpTransportMode::trusted_lan_http, std::nullopt};
        const auto context =
            sparenode::http::resolve_http_transport(policy, {"192.0.2.44", 1234}, request);
        REQUIRE(context.has_value());
        CHECK(context->client_address == "192.0.2.44");
        CHECK_FALSE(context->secure);
        CHECK_FALSE(context->trusted_proxy);
        CHECK_FALSE(sparenode::http::http_transport_allows_peer(policy, {"not-an-ip", 1234}));
    }
}

TEST_CASE("Trusted proxy mode requires one HTTPS scheme and one numeric client",
          "[http][transport][proxy]")
{
    using Code = sparenode::http::HttpTransportErrorCode;
    const sparenode::http::HttpTransportPolicy policy{
        sparenode::http::HttpTransportMode::trusted_proxy_https, "2001:db8::1"};

    CHECK(sparenode::http::http_transport_allows_peer(
        policy, {"2001:0db8:0000:0000:0000:0000:0000:0001", 443}));
    CHECK_FALSE(sparenode::http::http_transport_allows_peer(policy, {"2001:db8::2", 443}));

    const auto check_error = [&policy](const std::string_view headers, const Code expected)
    {
        const std::string source =
            "GET / HTTP/1.1\r\nHost: local\r\n" + std::string(headers) + "\r\n";
        const auto request = parse_request(source);
        const auto result =
            sparenode::http::resolve_http_transport(policy, {"2001:db8::1", 443}, request);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == expected);
    };

    check_error("X-Forwarded-For: 192.0.2.8\r\n", Code::missing_forwarded_scheme);
    check_error("X-Forwarded-Proto: https\r\n", Code::missing_forwarded_client);
    check_error("X-Forwarded-Proto: https\r\nX-Forwarded-Proto: https\r\n"
                "X-Forwarded-For: 192.0.2.8\r\n",
                Code::duplicate_forwarded_scheme);
    check_error("X-Forwarded-Proto: https\r\nX-Forwarded-For: 192.0.2.8\r\n"
                "X-Forwarded-For: 192.0.2.9\r\n",
                Code::duplicate_forwarded_client);
    check_error("X-Forwarded-Proto: http\r\nX-Forwarded-For: 192.0.2.8\r\n",
                Code::insecure_forwarded_scheme);
    check_error("X-Forwarded-Proto: https\r\nX-Forwarded-For: 192.0.2.8, 198.51.100.4\r\n",
                Code::invalid_forwarded_client);
    check_error("X-Forwarded-Proto: https\r\nX-Forwarded-For: client.example\r\n",
                Code::invalid_forwarded_client);

    constexpr std::string_view valid_source =
        "GET / HTTP/1.1\r\nHost: local\r\nX-Forwarded-Proto: HTTPS\r\n"
        "X-Forwarded-For: 192.0.2.8\r\n\r\n";
    const auto request = parse_request(valid_source);
    const auto context =
        sparenode::http::resolve_http_transport(policy, {"2001:db8::1", 443}, request);
    REQUIRE(context.has_value());
    CHECK(context->peer_address == "2001:db8::1");
    CHECK(context->client_address == "192.0.2.8");
    CHECK(context->secure);
    CHECK(context->trusted_proxy);
}

TEST_CASE("Trusted proxy resolution rejects a request from any other direct peer",
          "[http][transport][proxy]")
{
    constexpr std::string_view source =
        "GET / HTTP/1.1\r\nHost: local\r\nX-Forwarded-Proto: https\r\n"
        "X-Forwarded-For: 192.0.2.8\r\n\r\n";
    const auto request = parse_request(source);
    const sparenode::http::HttpTransportPolicy policy{
        sparenode::http::HttpTransportMode::trusted_proxy_https, "127.0.0.2"};

    const auto result =
        sparenode::http::resolve_http_transport(policy, {"127.0.0.1", 443}, request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == sparenode::http::HttpTransportErrorCode::peer_not_permitted);
}
