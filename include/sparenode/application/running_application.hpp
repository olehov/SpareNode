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

    /// @brief Transfers ownership of validated settings and all running servers.
    /// @param[in,out] other Application whose resources are transferred.
    RunningApplication(RunningApplication &&other) noexcept = default;

    /// @brief Stops currently owned servers before taking ownership from another application.
    /// @param[in,out] other Application whose resources are transferred.
    /// @return This application after the ownership transfer.
    RunningApplication &operator=(RunningApplication &&other) noexcept = default;
    RunningApplication(const RunningApplication &) = delete;
    RunningApplication &operator=(const RunningApplication &) = delete;

    /// @brief Returns the immutable runtime settings retained by the application.
    /// @return Validated configuration that produced the running servers.
    [[nodiscard]] const configuration::runtime::AppConfig &config() const noexcept;

    /// @brief Returns the RAII-owned running connection servers.
    /// @return Servers in the same order as their runtime configuration entries.
    [[nodiscard]] const std::vector<network::ConnectionServer> &servers() const noexcept;

  private:
    /// @brief Stores a successfully initialized application state.
    /// @param[in] config Validated settings retained for application consumers.
    /// @param[in] servers Running servers transferred into RAII ownership.
    RunningApplication(configuration::runtime::AppConfig config,
                       std::vector<network::ConnectionServer> servers) noexcept;

    configuration::runtime::AppConfig config_;       ///< Retained validated runtime settings.
    std::vector<network::ConnectionServer> servers_; ///< RAII-owned servers in config order.
};

/// @brief Describes one portable application startup failure category.
/// @param[in] code Failure category to describe.
/// @return Static English diagnostic text.
[[nodiscard]] const char *to_string(ApplicationStartErrorCode code) noexcept;

} // namespace sparenode::application
