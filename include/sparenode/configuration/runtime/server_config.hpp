#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "sparenode/configuration/runtime/share_config.hpp"
#include "sparenode/http/http_request_timeouts.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"

namespace sparenode::configuration::runtime
{

/// @brief Owns immutable validated settings consumed when a connection server starts.
class ServerConfig final
{
  public:
    /// @brief Creates one complete runtime server configuration.
    /// @param[in] endpoint Numeric listener address and TCP port.
    /// @param[in] multithreading_enabled Enables the configured fixed worker pool.
    /// @param[in] worker_threads Configured worker count.
    /// @param[in] minimum_log_severity Minimum emitted log severity.
    /// @param[in] shares Validated filesystem shares in configuration order.
    /// @param[in] http_timeouts Validated HTTP receive budgets.
    ServerConfig(network::TcpEndpoint endpoint, bool multithreading_enabled,
                 std::size_t worker_threads, logging::LogSeverity minimum_log_severity,
                 std::vector<ShareConfig> shares, http::HttpRequestTimeouts http_timeouts = {})
        : endpoint_(std::move(endpoint)), multithreading_enabled_(multithreading_enabled),
          worker_threads_(worker_threads), minimum_log_severity_(minimum_log_severity),
          shares_(std::move(shares)), http_timeouts_(http_timeouts)
    {
    }

    /// @brief Returns the listener endpoint.
    /// @return Immutable numeric address and port.
    [[nodiscard]] const network::TcpEndpoint &endpoint() const noexcept
    {
        return endpoint_;
    }

    /// @brief Reports whether the configured worker pool is enabled.
    /// @return `true` when more than the forced single worker may be used.
    [[nodiscard]] bool multithreading_enabled() const noexcept
    {
        return multithreading_enabled_;
    }

    /// @brief Returns the configured worker count.
    /// @return Worker count retained from validated configuration.
    [[nodiscard]] std::size_t worker_threads() const noexcept
    {
        return worker_threads_;
    }

    /// @brief Returns the minimum emitted log severity.
    /// @return Immutable configured log threshold.
    [[nodiscard]] logging::LogSeverity minimum_log_severity() const noexcept
    {
        return minimum_log_severity_;
    }

    /// @brief Returns the configured shares.
    /// @return Immutable shares in configuration order.
    [[nodiscard]] const std::vector<ShareConfig> &shares() const noexcept
    {
        return shares_;
    }

    /// @brief Resolves the worker count permitted by the threading switch.
    /// @return Configured worker count when enabled, otherwise exactly one worker.
    [[nodiscard]] constexpr std::size_t effective_worker_count() const noexcept
    {
        return multithreading_enabled_ ? worker_threads_ : 1;
    }

    /// @brief Returns the validated HTTP receive policy.
    /// @return Header/body inactivity and total receive budgets.
    [[nodiscard]] const http::HttpRequestTimeouts &http_timeouts() const noexcept
    {
        return http_timeouts_;
    }

  private:
    network::TcpEndpoint endpoint_;             ///< Numeric listener address and TCP port.
    bool multithreading_enabled_;               ///< Enables the configured fixed worker pool.
    std::size_t worker_threads_;                ///< Configured worker count.
    logging::LogSeverity minimum_log_severity_; ///< Minimum emitted log severity.
    std::vector<ShareConfig> shares_;           ///< Validated shares in configuration order.
    http::HttpRequestTimeouts http_timeouts_;   ///< Immutable HTTP receive budgets.
};

} // namespace sparenode::configuration::runtime
