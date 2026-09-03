#include "sparenode/logging/network_logging.hpp"

#include <sstream>
#include <utility>

namespace sparenode::logging
{

namespace
{

[[nodiscard]] const char *to_string(const network::NetworkOperation operation) noexcept
{
    switch (operation)
    {
    case network::NetworkOperation::initialize:
        return "initialize";
    case network::NetworkOperation::resolve_address:
        return "resolve_address";
    case network::NetworkOperation::create_socket:
        return "create_socket";
    case network::NetworkOperation::configure_socket:
        return "configure_socket";
    case network::NetworkOperation::bind:
        return "bind";
    case network::NetworkOperation::listen:
        return "listen";
    case network::NetworkOperation::accept:
        return "accept";
    case network::NetworkOperation::receive:
        return "receive";
    case network::NetworkOperation::send:
        return "send";
    case network::NetworkOperation::query_local_endpoint:
        return "query_local_endpoint";
    case network::NetworkOperation::query_peer_endpoint:
        return "query_peer_endpoint";
    }
    return "unknown";
}

[[nodiscard]] const char *to_string(const network::NetworkErrorDomain domain) noexcept
{
    switch (domain)
    {
    case network::NetworkErrorDomain::validation:
        return "validation";
    case network::NetworkErrorDomain::address_resolution:
        return "address_resolution";
    case network::NetworkErrorDomain::socket:
        return "socket";
    case network::NetworkErrorDomain::state:
        return "state";
    case network::NetworkErrorDomain::cancellation:
        return "cancellation";
    case network::NetworkErrorDomain::timeout:
        return "timeout";
    }
    return "unknown";
}

[[nodiscard]] const char *to_string(const network::DispatchErrorCode code) noexcept
{
    switch (code)
    {
    case network::DispatchErrorCode::invalid_worker_count:
        return "invalid_worker_count";
    case network::DispatchErrorCode::invalid_pending_connection_limit:
        return "invalid_pending_connection_limit";
    case network::DispatchErrorCode::missing_connection_handler:
        return "missing_connection_handler";
    case network::DispatchErrorCode::invalid_connection:
        return "invalid_connection";
    case network::DispatchErrorCode::stopped:
        return "stopped";
    case network::DispatchErrorCode::cancelled:
        return "cancelled";
    case network::DispatchErrorCode::worker_start_failed:
        return "worker_start_failed";
    case network::DispatchErrorCode::resource_allocation_failed:
        return "resource_allocation_failed";
    }
    return "unknown";
}

[[nodiscard]] std::string format_dispatch_error(const network::DispatchError &error)
{
    std::ostringstream output;
    output << "dispatch_code=" << to_string(error.code) << " native_code=" << error.native_code;
    return output.str();
}

/// @brief Formats one isolated connection-handler failure.
/// @param[in] failure Worker-boundary failure to describe.
/// @return Non-sensitive failure kind and all available structured details.
[[nodiscard]] std::string format_connection_failure(const network::ConnectionFailure &failure)
{
    if (failure.kind == network::ConnectionFailureKind::handler_error &&
        failure.network_error.has_value())
    {
        return "handler_error " + format_network_error(failure.network_error.value());
    }
    return "handler_exception";
}

/// @brief Formats one fatal connection-server lifecycle failure.
/// @param[in] failure Accept-thread boundary failure to describe.
/// @return Non-sensitive failure kind and all available structured details.
[[nodiscard]] std::string format_server_failure(const network::ConnectionServerFailure &failure)
{
    switch (failure.kind)
    {
    case network::ConnectionServerFailureKind::accept_error:
        if (failure.network_error.has_value())
        {
            return "accept_error " + format_network_error(failure.network_error.value());
        }
        return "accept_error";
    case network::ConnectionServerFailureKind::dispatch_error:
        if (failure.dispatch_error.has_value())
        {
            return "dispatch_error " + format_dispatch_error(failure.dispatch_error.value());
        }
        return "dispatch_error";
    case network::ConnectionServerFailureKind::internal_exception:
        return "internal_exception";
    }
    return "unknown_server_failure";
}

} // namespace

std::string format_network_error(const network::NetworkError &error)
{
    std::ostringstream output;
    output << "operation=" << to_string(error.operation) << " domain=" << to_string(error.domain)
           << " code=" << error.code;
    return output.str();
}

network::ConnectionFailureObserver make_connection_failure_log_observer(Logger logger,
                                                                        std::string subsystem)
{
    return [logger = std::move(logger),
            subsystem = std::move(subsystem)](const network::ConnectionFailure &failure) noexcept
    {
        try
        {
            logger.log({LogSeverity::error, subsystem, format_connection_failure(failure)});
        }
        catch (...)
        {
            // Observer diagnostics must not escape the worker thread boundary.
            return;
        }
    };
}

network::ConnectionServerFailureObserver
make_connection_server_failure_log_observer(Logger logger, std::string subsystem)
{
    return [logger = std::move(logger), subsystem = std::move(subsystem)](
               const network::ConnectionServerFailure &failure) noexcept
    {
        try
        {
            logger.log({LogSeverity::error, subsystem, format_server_failure(failure)});
        }
        catch (...)
        {
            // Observer diagnostics must not escape the accept thread boundary.
            return;
        }
    };
}

} // namespace sparenode::logging
