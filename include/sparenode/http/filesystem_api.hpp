#pragma once

#include <cstdint>

#include "sparenode/configuration/runtime/server_config.hpp"
#include "sparenode/http/http_router.hpp"
#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Identifies why the filesystem API router could not be constructed.
enum class FilesystemApiErrorCode : std::uint8_t
{
    missing_share,             ///< Runtime configuration contains no filesystem share.
    route_registration_failed, ///< A fixed filesystem API route could not be registered.
    resource_allocation_failed ///< Router-owned memory could not be allocated.
};

/// @brief Preserves filesystem API construction failure details.
struct FilesystemApiError
{
    FilesystemApiErrorCode code{};            ///< Stable construction failure category.
    HttpRouteRegistrationError route_error{}; ///< Optional route registration detail.
};

/// @brief Builds the immutable-ready version-one filesystem API routes.
/// @param[in] server Validated server whose sole share backs the endpoints.
/// @return Router containing directory listing endpoints, or a startup failure.
[[nodiscard]] Result<HttpRouter, FilesystemApiError>
make_filesystem_api_router(const configuration::runtime::ServerConfig &server);

/// @brief Returns stable text for one filesystem API construction failure.
/// @param[in] code Failure category to describe.
/// @return Static diagnostic text.
[[nodiscard]] const char *to_string(FilesystemApiErrorCode code) noexcept;

} // namespace sparenode::http
