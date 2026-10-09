#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/http/request/http_request.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Selects how authenticated HTTP traffic may reach SpareNode.
enum class HttpTransportMode : std::uint8_t
{
    loopback_http,       ///< Direct plaintext access is restricted to the local host.
    trusted_lan_http,    ///< Direct plaintext LAN access is an explicit deployment choice.
    trusted_proxy_https, ///< One configured proxy supplies validated HTTPS metadata.
};

/// @brief Defines the listener-facing trust boundary for HTTP requests.
struct HttpTransportPolicy
{
    HttpTransportMode mode{HttpTransportMode::loopback_http}; ///< Effective deployment mode.
    std::optional<std::string>
        trusted_proxy; ///< Sole numeric proxy address when proxy mode is used.
};

/// @brief Parses one persistent transport-mode spelling.
/// @param[in] value Case-sensitive configuration value.
/// @return Typed mode, or no value for unsupported text.
[[nodiscard]] std::optional<HttpTransportMode>
parse_http_transport_mode(std::string_view value) noexcept;

/// @brief Identifies why a request does not satisfy its configured transport boundary.
enum class HttpTransportErrorCode : std::uint8_t
{
    peer_not_permitted,         ///< The direct peer is outside the configured trust boundary.
    missing_forwarded_scheme,   ///< A trusted proxy omitted X-Forwarded-Proto.
    duplicate_forwarded_scheme, ///< More than one forwarded scheme value was supplied.
    insecure_forwarded_scheme,  ///< The trusted proxy reported a non-HTTPS client request.
    missing_forwarded_client,   ///< A trusted proxy omitted X-Forwarded-For.
    duplicate_forwarded_client, ///< More than one forwarded client value was supplied.
    invalid_forwarded_client,   ///< Forwarded client text is not one numeric IP address.
};

/// @brief Resolves trusted request transport metadata from the direct TCP peer and headers.
/// @param[in] policy Validated server transport policy.
/// @param[in] peer Direct numeric endpoint captured by the accepted socket.
/// @param[in] request Complete parsed request with bounded headers.
/// @return Effective context, or a fail-closed policy error.
[[nodiscard]] Result<HttpRequestTransportContext, HttpTransportErrorCode>
resolve_http_transport(const HttpTransportPolicy &policy, const network::TcpEndpoint &peer,
                       const HttpRequestView &request);

/// @brief Checks whether a direct peer may enter the configured HTTP request pipeline.
/// @param[in] policy Validated server transport policy.
/// @param[in] peer Direct numeric endpoint captured by the accepted socket.
/// @return `true` when the peer belongs to the selected deployment boundary.
[[nodiscard]] bool http_transport_allows_peer(const HttpTransportPolicy &policy,
                                              const network::TcpEndpoint &peer) noexcept;

} // namespace sparenode::http
