#include "sparenode/http/session/http_transport_policy.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include "sparenode/http/detail/ascii.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace sparenode::http
{
namespace
{

/// @brief Stores one parsed numeric address without exposing platform headers publicly.
struct ParsedIpAddress
{
    int family{};                          ///< AF_INET or AF_INET6.
    std::array<unsigned char, 16> bytes{}; ///< Network-order address bytes.
};

/// @brief Parses exactly one numeric IPv4 or IPv6 address.
[[nodiscard]] std::optional<ParsedIpAddress> parse_ip_address(const std::string_view text) noexcept
{
    constexpr std::size_t maximum_address_characters = INET6_ADDRSTRLEN - 1;
    if (text.empty() || text.size() > maximum_address_characters ||
        text.find('\0') != std::string_view::npos)
    {
        return std::nullopt;
    }
    std::array<char, INET6_ADDRSTRLEN> value{};
    std::ranges::copy(text, value.begin());
    ParsedIpAddress result;
    in_addr ipv4{};
#ifdef _WIN32
    if (InetPtonA(AF_INET, value.data(), &ipv4) == 1)
#else
    if (inet_pton(AF_INET, value.data(), &ipv4) == 1)
#endif
    {
        result.family = AF_INET;
        const auto *bytes = reinterpret_cast<const unsigned char *>(&ipv4);
        std::copy_n(bytes, sizeof(ipv4), result.bytes.begin());
        return result;
    }

    in6_addr ipv6{};
#ifdef _WIN32
    if (InetPtonA(AF_INET6, value.data(), &ipv6) == 1)
#else
    if (inet_pton(AF_INET6, value.data(), &ipv6) == 1)
#endif
    {
        result.family = AF_INET6;
        const auto *bytes = reinterpret_cast<const unsigned char *>(&ipv6);
        std::copy_n(bytes, result.bytes.size(), result.bytes.begin());
        return result;
    }
    return std::nullopt;
}

/// @brief Reports whether one numeric address belongs to the host loopback range.
[[nodiscard]] bool is_loopback_address(const std::string_view address) noexcept
{
    const auto parsed = parse_ip_address(address);
    if (!parsed)
    {
        return false;
    }
    if (parsed->family == AF_INET)
    {
        constexpr unsigned char ipv4_loopback_prefix = 127;
        return parsed->bytes.front() == ipv4_loopback_prefix;
    }
    const auto last_byte = parsed->bytes.end() - 1;
    return std::ranges::all_of(parsed->bytes.begin(), last_byte,
                               [](const unsigned char byte) { return byte == 0; }) &&
           *last_byte == 1;
}

} // namespace

std::optional<HttpTransportMode> parse_http_transport_mode(const std::string_view value) noexcept
{
    if (value == "loopback_http")
    {
        return HttpTransportMode::loopback_http;
    }
    if (value == "trusted_lan_http")
    {
        return HttpTransportMode::trusted_lan_http;
    }
    if (value == "trusted_proxy_https")
    {
        return HttpTransportMode::trusted_proxy_https;
    }
    return std::nullopt;
}

bool http_transport_allows_peer(const HttpTransportPolicy &policy,
                                const network::TcpEndpoint &peer) noexcept
{
    switch (policy.mode)
    {
    case HttpTransportMode::loopback_http:
        return is_loopback_address(peer.address);
    case HttpTransportMode::trusted_lan_http:
        return parse_ip_address(peer.address).has_value();
    case HttpTransportMode::trusted_proxy_https:
    {
        if (!policy.trusted_proxy)
        {
            return false;
        }
        const auto parsed_peer = parse_ip_address(peer.address);
        const auto parsed_proxy = parse_ip_address(*policy.trusted_proxy);
        return parsed_peer.has_value() && parsed_proxy.has_value() &&
               parsed_peer->family == parsed_proxy->family &&
               parsed_peer->bytes == parsed_proxy->bytes;
    }
    }
    return false;
}

Result<HttpRequestTransportContext, HttpTransportErrorCode>
resolve_http_transport(const HttpTransportPolicy &policy, const network::TcpEndpoint &peer,
                       const HttpRequestView &request)
{
    if (!http_transport_allows_peer(policy, peer))
    {
        return unexpected(HttpTransportErrorCode::peer_not_permitted);
    }

    HttpRequestTransportContext context{peer.address, peer.address, false, false};
    if (policy.mode != HttpTransportMode::trusted_proxy_https)
    {
        return context;
    }

    const auto schemes = request.headers("X-Forwarded-Proto");
    if (schemes.empty())
    {
        return unexpected(HttpTransportErrorCode::missing_forwarded_scheme);
    }
    if (schemes.size() != 1)
    {
        return unexpected(HttpTransportErrorCode::duplicate_forwarded_scheme);
    }
    if (!detail::ascii_case_insensitive_equal(schemes.front(), "https"))
    {
        return unexpected(HttpTransportErrorCode::insecure_forwarded_scheme);
    }

    const auto clients = request.headers("X-Forwarded-For");
    if (clients.empty())
    {
        return unexpected(HttpTransportErrorCode::missing_forwarded_client);
    }
    if (clients.size() != 1)
    {
        return unexpected(HttpTransportErrorCode::duplicate_forwarded_client);
    }
    if (!parse_ip_address(clients.front()).has_value())
    {
        return unexpected(HttpTransportErrorCode::invalid_forwarded_client);
    }

    context.client_address = std::string(clients.front());
    context.secure = true;
    context.trusted_proxy = true;
    return context;
}

} // namespace sparenode::http
