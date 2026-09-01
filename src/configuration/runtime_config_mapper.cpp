#include "sparenode/configuration/runtime_config_mapper.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "sparenode/configuration/directives/parsed_server_directive.hpp"
#include "sparenode/configuration/directives/parsed_share_directive.hpp"
#include "sparenode/logging/log_severity.hpp"

namespace sparenode::configuration
{
namespace
{

using directives::ServerDirectiveKind;
using directives::ShareDirectiveKind;

/// @brief Holds mapper-local server values before immutable runtime construction.
struct ServerValues
{
    network::TcpEndpoint endpoint{"0.0.0.0", 8080}; ///< Default listener endpoint.
    bool multithreading_enabled{false};             ///< Default single-worker switch.
    std::size_t worker_threads{1};                  ///< Default worker count.
    logging::LogSeverity minimum_log_severity{logging::LogSeverity::info}; ///< Default threshold.
};

/// @brief Holds mapper-local permission values before immutable runtime construction.
struct PermissionValues
{
    bool allow_read{true};    ///< Default read permission.
    bool allow_write{false};  ///< Default write permission.
    bool allow_delete{false}; ///< Default delete permission.
};

/// @brief Applies one validated server directive to its runtime destination.
/// @param[in] directive Parsed directive whose semantic constraints already passed.
/// @param[in,out] server Runtime server settings receiving the mapped value.
void apply_server_directive(const directives::ParsedServerDirective &directive,
                            ServerValues &server)
{
    switch (directive.kind)
    {
    case ServerDirectiveKind::bind:
        server.endpoint.address = std::get<std::string>(directive.value.scalar);
        break;
    case ServerDirectiveKind::port:
        server.endpoint.port =
            static_cast<std::uint16_t>(std::get<std::uint64_t>(directive.value.scalar));
        break;
    case ServerDirectiveKind::multithreading:
        server.multithreading_enabled = std::get<bool>(directive.value.scalar);
        break;
    case ServerDirectiveKind::worker_threads:
        server.worker_threads =
            static_cast<std::size_t>(std::get<std::uint64_t>(directive.value.scalar));
        break;
    case ServerDirectiveKind::log_level:
        if (const auto severity =
                logging::parse_log_severity(std::get<std::string>(directive.value.scalar)))
        {
            server.minimum_log_severity = *severity;
        }
        break;
    }
}

/// @brief Applies one validated share permission while ignoring its consumed path.
/// @param[in] directive Parsed directive whose semantic constraints already passed.
/// @param[in,out] permissions Runtime permission flags receiving the mapped value.
void apply_share_directive(const directives::ParsedShareDirective &directive,
                           PermissionValues &permissions)
{
    switch (directive.kind)
    {
    case ShareDirectiveKind::path:
        break;
    case ShareDirectiveKind::read_permission:
        permissions.allow_read = std::get<bool>(directive.value.scalar);
        break;
    case ShareDirectiveKind::write_permission:
        permissions.allow_write = std::get<bool>(directive.value.scalar);
        break;
    case ShareDirectiveKind::delete_permission:
        permissions.allow_delete = std::get<bool>(directive.value.scalar);
        break;
    }
}

} // namespace

runtime::AppConfig RuntimeConfigMapper::map(const ValidatedConfiguration &configuration)
{
    ServerValues server;
    const auto &parsed_server = configuration.parsed().server;
    for (const auto &directive : parsed_server.directives)
    {
        apply_server_directive(directive, server);
    }

    const auto &roots = configuration.shared_roots();
    std::vector<runtime::ShareConfig> shares;
    shares.reserve(parsed_server.shares.size());
    for (std::size_t index = 0; index < parsed_server.shares.size(); ++index)
    {
        const auto &parsed_share = parsed_server.shares[index];
        PermissionValues permissions;
        for (const auto &directive : parsed_share.directives)
        {
            apply_share_directive(directive, permissions);
        }
        shares.emplace_back(parsed_share.name, roots[index],
                            runtime::SharePermissions{permissions.allow_read,
                                                      permissions.allow_write,
                                                      permissions.allow_delete});
    }
    std::vector<runtime::ServerConfig> servers;
    servers.emplace_back(std::move(server.endpoint), server.multithreading_enabled,
                         server.worker_threads, server.minimum_log_severity, std::move(shares));
    return runtime::AppConfig(std::move(servers));
}

} // namespace sparenode::configuration
