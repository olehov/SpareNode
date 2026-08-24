#pragma once

#include <string>
#include <string_view>

#include "sparenode/logging/logger.hpp"
#include "sparenode/network/connection_dispatcher.hpp"
#include "sparenode/network/connection_server.hpp"
#include "sparenode/network/network_error.hpp"

namespace sparenode::logging
{

/// @brief Formats every field of a structured network error for diagnostics.
/// @param[in] error Network failure whose operation, domain, and code are preserved.
/// @return Non-sensitive diagnostic text containing every structured field.
[[nodiscard]] std::string format_network_error(const network::NetworkError &error);

/// @brief Creates a dispatcher observer that reports isolated handler failures.
/// @param[in] logger Logger copied safely into the concurrent observer.
/// @param[in] subsystem Stable category attached to emitted records.
/// @return Observer compatible with `ConnectionDispatcherConfig`.
[[nodiscard]] network::ConnectionFailureObserver
make_connection_failure_log_observer(Logger logger, std::string subsystem = "network.dispatcher");

/// @brief Creates a server observer that reports fatal accept-loop failures.
/// @param[in] logger Logger copied safely into the accept-thread observer.
/// @param[in] subsystem Stable category attached to emitted records.
/// @return Observer compatible with `ConnectionServerConfig`.
[[nodiscard]] network::ConnectionServerFailureObserver
make_connection_server_failure_log_observer(Logger logger,
                                            std::string subsystem = "network.server");

} // namespace sparenode::logging
