#pragma once

#include <cstddef>
#include <vector>

#include "sparenode/configuration/runtime/share_config.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Stores validated settings consumed when the connection server starts.
struct ServerConfig
{
    network::TcpEndpoint endpoint{"0.0.0.0", 8080}; ///< Numeric listener address and TCP port.
    bool multithreading_enabled{false};             ///< Enables the configured fixed worker pool.
    std::size_t worker_threads{1}; ///< Configured workers; one when threading is disabled.
    logging::LogSeverity minimum_log_severity{logging::LogSeverity::info}; ///< Log threshold.
    std::vector<ShareConfig> shares; ///< Validated filesystem shares in configured order.

    /// @brief Resolves the worker count permitted by the threading switch.
    /// @return Configured worker count when enabled, otherwise exactly one worker.
    [[nodiscard]] constexpr std::size_t effective_worker_count() const noexcept
    {
        return multithreading_enabled ? worker_threads : 1;
    }
};

} // namespace sparenode::configuration::runtime
