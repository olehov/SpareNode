#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/mime_type_registry.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http
{

/// @brief Identifies why the filesystem API router could not be constructed.
enum class FilesystemApiErrorCode : std::uint8_t
{
    missing_location,          ///< API settings contain no filesystem location.
    route_registration_failed, ///< A configured filesystem API route could not be registered.
    resource_allocation_failed ///< Router-owned memory could not be allocated.
};

/// @brief Preserves filesystem API construction failure details.
struct FilesystemApiError
{
    FilesystemApiErrorCode code{};            ///< Stable construction failure category.
    HttpRouteRegistrationError route_error{}; ///< Optional route registration detail.
};

/// @brief Owns the filesystem-specific settings for one exposed HTTP location.
struct FilesystemApiLocation
{
    std::string api_path;                        ///< Validated public HTTP path prefix.
    filesystem::SharedRoot root;                 ///< Canonical filesystem security boundary.
    filesystem::LocationPermissions permissions; ///< Operations enabled for the location.
};

/// @brief Owns only the settings required to compose the filesystem API.
struct FilesystemApiSettings
{
    std::vector<FilesystemApiLocation> locations; ///< Filesystem locations in route order.
    MimeTypeRegistry mime_types;                  ///< Immutable response media-type mappings.
};

/// @brief Builds the immutable-ready version-one filesystem API routes.
/// @param[in] settings Filesystem locations and MIME mappings consumed by the handlers.
/// @param[in] protected_route_decorator Optional access policy applied to every endpoint.
/// @return Router containing configured filesystem endpoints, or a startup failure.
[[nodiscard]] Result<HttpRouter, FilesystemApiError>
make_filesystem_api_router(FilesystemApiSettings settings,
                           api::ProtectedRouteDecorator protected_route_decorator = {});

/// @brief Returns stable text for one filesystem API construction failure.
/// @param[in] code Failure category to describe.
/// @return Static diagnostic text.
[[nodiscard]] const char *to_string(FilesystemApiErrorCode code) noexcept;

} // namespace sparenode::http
