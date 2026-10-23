#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "sparenode/application/runtime/app_config.hpp"
#include "sparenode/application/session_authentication_service.hpp"
#include "sparenode/authentication/authentication_service.hpp"
#include "sparenode/authentication/file_authentication.hpp"
#include "sparenode/bootstrap/mime_types_loader.hpp"
#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/session/session_store.hpp"

namespace sparenode::bootstrap
{

/// @brief Identifies an environment-dependent construction failure before startup.
enum class RuntimeBootstrapErrorCode : std::uint8_t
{
    invalid_location_root,         ///< A configured shared root could not be constructed.
    mime_types_load_failed,        ///< An external MIME mapping could not be loaded.
    authentication_failed,         ///< The credential-backed authentication stack failed closed.
    session_initialization_failed, ///< The bounded session service could not be constructed.
    resource_allocation_failed     ///< Bootstrap-owned storage could not be allocated.
};

/// @brief Preserves structured bootstrap detail without exposing configuration secrets.
// The analyzer loses the default member initializer while tracing std::expected::error() copies.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct RuntimeBootstrapError
{
    RuntimeBootstrapErrorCode code{
        RuntimeBootstrapErrorCode::invalid_location_root}; ///< Stable construction category.
    std::filesystem::path source_path;      ///< Main or external source associated with failure.
    configuration::SourceLocation location; ///< Related configuration literal when available.
    std::optional<filesystem::SharedRootError> shared_root_error; ///< Root construction detail.
    std::optional<MimeTypesLoadError> mime_types_error;           ///< External MIME source detail.
    std::optional<authentication::FileAuthenticationErrorCode>
        authentication_error; ///< Non-sensitive credential construction detail.
};

/// @brief Owns every environment-dependent value created before listener startup.
class RuntimePlan final
{
  public:
    /// @brief Stores fully constructed runtime settings and authentication.
    /// @param[in] config Concrete application settings created by bootstrap.
    /// @param[in] session_authentication Ready login and session-lifecycle application service.
    /// @param[in] process_log_severity Process lifecycle logging threshold.
    RuntimePlan(application::runtime::AppConfig config,
                application::SessionAuthenticationService session_authentication,
                logging::LogSeverity process_log_severity) noexcept;

    /// @brief Returns constructed application settings without transferring ownership.
    /// @return Immutable concrete application settings.
    [[nodiscard]] const application::runtime::AppConfig &config() const noexcept;
    /// @brief Returns the process-wide authentication service retained for composition.
    ///
    /// Version one has one owner credential realm. Per-server route and transport
    /// policies may differ, but they refer to this single process-lifetime service.
    /// @return Immutable ready-to-use authentication service.
    [[nodiscard]] const authentication::AuthenticationService &authentication() const noexcept;

    /// @brief Returns the process-wide login and session-lifecycle application service.
    /// @return Immutable service whose operations mutate only the shared bounded store.
    [[nodiscard]] const application::SessionAuthenticationService &
    session_authentication() const noexcept;

    /// @brief Shares the process-wide bounded session service with API composition.
    /// @return Shared mutable session service owned by the runtime plan.
    [[nodiscard]] const std::shared_ptr<session::SessionStore> &sessions() const noexcept;

    /// @brief Returns the process lifecycle logging threshold.
    /// @return Severity derived from the single version-one server configuration.
    [[nodiscard]] logging::LogSeverity process_log_severity() const noexcept;

  private:
    application::runtime::AppConfig config_; ///< Concrete settings ready for application startup.
    application::SessionAuthenticationService
        session_authentication_; ///< Ready login and session-lifecycle orchestration.
    logging::LogSeverity process_log_severity_; ///< Process-wide lifecycle logging threshold.
};

/// @brief Constructs concrete runtime dependencies from pure validated configuration settings.
class RuntimeBootstrap final
{
  public:
    /// @brief Resolves external paths and builds all resources before any listener starts.
    /// @param[in] loaded Validated primitive settings and their source path.
    /// @return A complete runtime plan, or a structured fail-closed bootstrap error.
    [[nodiscard]] static Result<RuntimePlan, RuntimeBootstrapError>
    build(const configuration::LoadedConfiguration &loaded);
};

/// @brief Formats a non-sensitive bootstrap diagnostic with retained source information.
[[nodiscard]] std::string format_runtime_bootstrap_error(const RuntimeBootstrapError &error);

/// @brief Returns stable text for one bootstrap failure category.
[[nodiscard]] const char *to_string(RuntimeBootstrapErrorCode code) noexcept;

} // namespace sparenode::bootstrap
