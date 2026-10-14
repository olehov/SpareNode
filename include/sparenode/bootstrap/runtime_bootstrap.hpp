#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "sparenode/application/runtime/app_config.hpp"
#include "sparenode/authentication/authentication_service.hpp"
#include "sparenode/authentication/file_authentication.hpp"
#include "sparenode/bootstrap/mime_types_loader.hpp"
#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "sparenode/foundation/result.hpp"

namespace sparenode::bootstrap
{

/// @brief Identifies an environment-dependent construction failure before startup.
enum class RuntimeBootstrapErrorCode : std::uint8_t
{
    invalid_location_root,     ///< A configured shared root could not be constructed.
    mime_types_load_failed,    ///< An external MIME mapping could not be loaded.
    authentication_failed,     ///< The credential-backed authentication stack failed closed.
    resource_allocation_failed ///< Bootstrap-owned storage could not be allocated.
};

/// @brief Preserves structured bootstrap detail without exposing configuration secrets.
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
    /// @param[in] authentication Ready credential-independent authentication service.
    RuntimePlan(application::runtime::AppConfig config,
                authentication::AuthenticationService authentication) noexcept;

    /// @brief Returns constructed application settings without transferring ownership.
    /// @return Immutable concrete application settings.
    [[nodiscard]] const application::runtime::AppConfig &config() const noexcept;
    /// @brief Transfers constructed application settings to the runtime owner.
    /// @return Concrete application settings moved out of this plan.
    [[nodiscard]] application::runtime::AppConfig release_config() && noexcept;
    /// @brief Returns the authentication service retained for application composition.
    /// @return Immutable ready-to-use authentication service.
    [[nodiscard]] const authentication::AuthenticationService &authentication() const noexcept;

  private:
    application::runtime::AppConfig config_; ///< Concrete settings ready for application startup.
    authentication::AuthenticationService authentication_; ///< Ready authentication service.
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
