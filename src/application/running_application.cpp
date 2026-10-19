#include "sparenode/application/running_application.hpp"

#include <new>
#include <sstream>
#include <utility>

namespace sparenode::application
{

/// @brief Starts complete server plans in order and rolls back on partial failure.
Result<RunningApplication, ApplicationStartError>
RunningApplication::start(std::vector<ServerRuntimePlan> plans)
{
    if (plans.empty())
    {
        return unexpected(ApplicationStartError{ApplicationStartErrorCode::missing_server, 0, {}});
    }
    try
    {
        std::vector<network::ConnectionServer> servers;
        servers.reserve(plans.size());
        for (std::size_t index = 0; index < plans.size(); ++index)
        {
            auto server_result =
                network::ConnectionServer::start(std::move(plans[index]).release_config());
            if (!server_result)
            {
                return unexpected(ApplicationStartError{
                    ApplicationStartErrorCode::server_start_failed, index, server_result.error()});
            }
            servers.push_back(std::move(server_result).value());
        }
        return RunningApplication(std::move(servers));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            ApplicationStartError{ApplicationStartErrorCode::resource_allocation_failed, 0, {}});
    }
}

/// @brief Takes ownership of all successfully started connection servers.
RunningApplication::RunningApplication(std::vector<network::ConnectionServer> servers) noexcept
    : servers_(std::move(servers))
{
}

/// @brief Exposes the running servers without transferring ownership.
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
