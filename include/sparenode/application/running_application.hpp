#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "sparenode/configuration/runtime/app_config.hpp"
#include "sparenode/network/connection_server.hpp"
#include "sparenode/result.hpp"

namespace sparenode::application
{

/// @brief Identifies why validated runtime settings could not start the application.
enum class ApplicationStartErrorCode : std::uint8_t
{
    missing_server,             ///< No runtime server settings were supplied.
    server_start_failed,        ///< One configured connection server could not start.
    resource_allocation_failed, ///< Application-owned server storage could not be allocated.
};

/// @brief Preserves the server index and native startup details of a failed start.
struct ApplicationStartError
{
    ApplicationStartErrorCode code{}; ///< Portable application startup category.
    std::size_t server_index{};       ///< Configured server associated with the failure.
    std::optional<network::ConnectionServerStartError> server_error; ///< Network detail.
};

/// @brief Owns validated settings and every connection server started from them.
class RunningApplication final
{
  public:
    /// @brief Starts all configured servers using one shared connection handler.
    /// @param[in] config Validated parser-independent application settings.
    /// @param[in] handler Handler copied into each configured server.
    /// @return Fully running RAII owner, or a structured startup failure.
    [[nodiscard]] static Result<RunningApplication, ApplicationStartError>
    start(configuration::runtime::AppConfig config, network::ConnectionHandler handler);

    RunningApplication(RunningApplication &&) noexcept = default;
    RunningApplication &operator=(RunningApplication &&) noexcept = default;
    RunningApplication(const RunningApplication &) = delete;
    RunningApplication &operator=(const RunningApplication &) = delete;

    /// @brief Returns the immutable runtime settings retained by the application.
    [[nodiscard]] const configuration::runtime::AppConfig &config() const noexcept;

    /// @brief Returns the RAII-owned running connection servers.
    [[nodiscard]] const std::vector<network::ConnectionServer> &servers() const noexcept;

  private:
    RunningApplication(configuration::runtime::AppConfig config,
                       std::vector<network::ConnectionServer> servers) noexcept;

    configuration::runtime::AppConfig config_;
    std::vector<network::ConnectionServer> servers_;
};

[[nodiscard]] const char *to_string(ApplicationStartErrorCode code) noexcept;

} // namespace sparenode::application
