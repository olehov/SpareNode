#include "sparenode/application/running_application.hpp"

#include <new>
#include <sstream>
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
                          network::ConnectionHandler handler, RunningApplicationObservers observers)
{
    if (config.servers().empty())
    {
        return unexpected(ApplicationStartError{ApplicationStartErrorCode::missing_server, 0, {}});
    }
    try
    {
        std::vector<network::ConnectionServer> servers;
        servers.reserve(config.servers().size());
        for (std::size_t index = 0; index < config.servers().size(); ++index)
        {
            const auto &settings = config.servers()[index];
            network::ConnectionServerConfig server_config{
                settings.endpoint(),
                listen_backlog,
                settings.multithreading_enabled(),
                {{settings.worker_threads(), pending_connection_limit},
                 handler,
                 observers.connection_failure},
                observers.server_failure};
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

std::string format_application_start_error(const ApplicationStartError &error)
{
    std::ostringstream output;
    output << to_string(error.code) << " server_index=" << error.server_index;
    if (!error.server_error.has_value())
    {
        return output.str();
    }

    const network::ConnectionServerStartError &server_error = error.server_error.value();
    output << " server_error_code="
           << static_cast<unsigned int>(std::to_underlying(server_error.code))
           << " native_code=" << server_error.native_code;
    if (server_error.network_error.has_value())
    {
        const network::NetworkError &network_error = server_error.network_error.value();
        output << " network_operation="
               << static_cast<unsigned int>(std::to_underlying(network_error.operation))
               << " network_domain="
               << static_cast<unsigned int>(std::to_underlying(network_error.domain))
               << " network_code=" << network_error.code;
    }
    if (server_error.dispatch_error.has_value())
    {
        const network::DispatchError &dispatch_error = server_error.dispatch_error.value();
        output << " dispatch_code="
               << static_cast<unsigned int>(std::to_underlying(dispatch_error.code))
               << " dispatch_native_code=" << dispatch_error.native_code;
    }
    return output.str();
}

} // namespace sparenode::application
