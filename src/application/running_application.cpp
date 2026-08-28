#include "sparenode/application/running_application.hpp"

#include <new>
#include <utility>

namespace sparenode::application
{
namespace
{
/// @brief Conservative MVP limit for connections waiting in the operating system before accept().
constexpr int listen_backlog = 128;

/// @brief Conservative MVP limit for accepted connections waiting for a dispatcher worker.
///
/// This is not the total client limit: workers may own active connections while
/// the operating-system backlog holds additional connections that have not yet
/// been accepted. Keeping the queue bounded provides backpressure and prevents
/// untrusted clients from causing unbounded socket and memory growth.
constexpr std::size_t pending_connection_limit = 128;
} // namespace

Result<RunningApplication, ApplicationStartError>
RunningApplication::start(configuration::runtime::AppConfig config,
                          network::ConnectionHandler handler)
{
    if (config.servers.empty())
    {
        return unexpected(ApplicationStartError{ApplicationStartErrorCode::missing_server, 0, {}});
    }
    try
    {
        std::vector<network::ConnectionServer> servers;
        servers.reserve(config.servers.size());
        for (std::size_t index = 0; index < config.servers.size(); ++index)
        {
            const auto &settings = config.servers[index];
            network::ConnectionServerConfig server_config{
                settings.endpoint,
                listen_backlog,
                settings.multithreading_enabled,
                {{settings.worker_threads, pending_connection_limit}, handler, {}},
                {}};
            auto server_result = network::ConnectionServer::start(std::move(server_config));
            if (!server_result)
            {
                return unexpected(ApplicationStartError{
                    ApplicationStartErrorCode::server_start_failed, index, server_result.error()});
            }
            servers.push_back(std::move(server_result).value());
        }
        return RunningApplication(std::move(config), std::move(servers));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            ApplicationStartError{ApplicationStartErrorCode::resource_allocation_failed, 0, {}});
    }
}

RunningApplication::RunningApplication(configuration::runtime::AppConfig config,
                                       std::vector<network::ConnectionServer> servers) noexcept
    : config_(std::move(config)), servers_(std::move(servers))
{
}

const configuration::runtime::AppConfig &RunningApplication::config() const noexcept
{
    return config_;
}

const std::vector<network::ConnectionServer> &RunningApplication::servers() const noexcept
{
    return servers_;
}

const char *to_string(const ApplicationStartErrorCode code) noexcept
{
    switch (code)
    {
    case ApplicationStartErrorCode::missing_server:
        return "runtime configuration contains no server";
    case ApplicationStartErrorCode::server_start_failed:
        return "configured server could not be started";
    case ApplicationStartErrorCode::resource_allocation_failed:
        return "application server resources could not be allocated";
    }
    return "unknown application startup error";
}

} // namespace sparenode::application
