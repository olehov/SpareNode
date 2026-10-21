#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration
{

/// @brief Logging severities accepted by persistent configuration syntax.
enum class ConfiguredLogLevel : std::uint8_t
{
    debug,   ///< Diagnostic detail useful during development.
    info,    ///< Normal lifecycle and operational events.
    warning, ///< Recoverable conditions requiring attention.
    error    ///< Failed operations and unavailable components.
};

/// @brief Deployment boundaries accepted by persistent configuration syntax.
enum class ConfiguredTransportMode : std::uint8_t
{
    loopback_http,      ///< Plain HTTP restricted to local loopback peers.
    trusted_lan_http,   ///< Explicit plain HTTP exposure to trusted LAN peers.
    trusted_proxy_https ///< Effective HTTPS asserted by one trusted reverse proxy.
};

/// @brief Retains one configured path and the literal that supplied it.
struct ConfiguredPath
{
    std::string value;       ///< Decoded UTF-8 path text.
    SourceLocation location; ///< Location of the path literal.
};

/// @brief Contains validated primitive settings for one filesystem location.
struct LocationSettings
{
    std::string api_path;     ///< Validated HTTP origin path.
    ConfiguredPath root;      ///< Filesystem path constructed later by bootstrap.
    bool allow_read{true};    ///< Permits file and directory reads.
    bool allow_write{false};  ///< Permits uploads and directory creation.
    bool allow_delete{false}; ///< Permits file and directory deletion.
};

/// @brief Contains validated primitive settings for one server block.
struct ServerSettings
{
    SourceLocation location;               ///< Location of the containing server block.
    std::string bind_address{"127.0.0.1"}; ///< Validated numeric listener address.
    std::uint16_t port{8080};              ///< Nonzero listener port.
    bool multithreading_enabled{false};    ///< Enables the configured worker pool.
    std::size_t worker_threads{1};         ///< Worker count retained from configuration.
    ConfiguredLogLevel log_level{ConfiguredLogLevel::info}; ///< Configured log threshold.
    std::uint64_t header_timeout_ms{10'000};                ///< Header inactivity budget.
    std::uint64_t body_timeout_ms{10'000};                  ///< Body inactivity budget.
    std::uint64_t request_timeout_ms{30'000};               ///< Total request receive budget.
    ConfiguredTransportMode transport_mode{
        ConfiguredTransportMode::loopback_http};    ///< Configured deployment boundary.
    std::optional<std::string> trusted_proxy;       ///< Numeric trusted proxy when configured.
    std::optional<ConfiguredPath> mime_types_file;  ///< Optional external MIME mapping path.
    std::optional<ConfiguredPath> credentials_file; ///< Optional owner credential path.
    std::vector<LocationSettings> locations;        ///< Validated locations in source order.
};

/// @brief Owns validated implementation-independent settings for one process.
struct AppSettings
{
    std::vector<ServerSettings> servers; ///< Validated servers in source order.
};

} // namespace sparenode::configuration
