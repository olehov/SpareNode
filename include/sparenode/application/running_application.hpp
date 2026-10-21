#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sparenode/application/server_runtime_plan.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/network/connection_server.hpp"

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

/// @brief Owns every connection server started from fully composed runtime plans.
class RunningApplication final
{
  public:
    /// @brief Starts all independently composed server plans in their declared order.
    /// @param[in] plans Complete per-server configurations transferred into the runtime.
    /// @return Fully running RAII owner, or a structured startup failure.
    [[nodiscard]] static Result<RunningApplication, ApplicationStartError>
    start(std::vector<ServerRuntimePlan> plans);

    /// @brief Transfers ownership of validated settings and all running servers.
    /// @param[in,out] other Application whose resources are transferred.
    RunningApplication(RunningApplication &&other) noexcept = default;

    /// @brief Stops currently owned servers before taking ownership from another application.
    /// @param[in,out] other Application whose resources are transferred.
    /// @return This application after the ownership transfer.
    RunningApplication &operator=(RunningApplication &&other) noexcept = default;
    RunningApplication(const RunningApplication &) = delete;
    RunningApplication &operator=(const RunningApplication &) = delete;

    /// @brief Returns the RAII-owned running connection servers.
    /// @return Servers in the same order as their supplied plans.
    [[nodiscard]] const std::vector<network::ConnectionServer> &servers() const noexcept;

  private:
    /// @brief Stores a successfully initialized application state.
    /// @param[in] servers Running servers transferred into RAII ownership.
    explicit RunningApplication(std::vector<network::ConnectionServer> servers) noexcept;

    std::vector<network::ConnectionServer> servers_; ///< RAII-owned servers in config order.
};

/// @brief Describes one portable application startup failure category.
/// @param[in] code Failure category to describe.
/// @return Static English diagnostic text.
[[nodiscard]] const char *to_string(ApplicationStartErrorCode code) noexcept;

/// @brief Formats every structured application startup failure field.
/// @param[in] error Startup failure whose server and native details must be preserved.
/// @return Non-sensitive diagnostic text suitable for the startup error boundary.
[[nodiscard]] std::string format_application_start_error(const ApplicationStartError &error);

} // namespace sparenode::application
