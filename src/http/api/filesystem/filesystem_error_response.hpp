#pragma once

#include "sparenode/application/filesystem_service.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http::detail
{

/// @brief Converts one transport-neutral read failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemReadError &error);

/// @brief Converts one transport-neutral upload failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemUploadError &error);

/// @brief Converts one transport-neutral directory-creation failure into a public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemDirectoryCreationError &error);

/// @brief Converts one transport-neutral removal failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemRemovalError &error);

} // namespace sparenode::http::detail
