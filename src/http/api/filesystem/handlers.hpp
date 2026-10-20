#pragma once

#include <string_view>

#include "sparenode/application/filesystem_service.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/http/mime_type_registry.hpp"
#include "sparenode/http/request/http_request.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http::detail
{

/// @brief Handles one exact or wildcard directory-listing or inline-file route.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_resource(const application::FilesystemService &service, const MimeTypeRegistry &mime_types,
                std::string_view requested_path);

/// @brief Converts one opened file and request path into a streaming response.
[[nodiscard]] Result<HttpResponse, HttpRouteError> handle_file(filesystem::FileReadStream stream,
                                                               const MimeTypeRegistry &mime_types,
                                                               std::string_view requested_path);

/// @brief Publishes one complete decoded request body under a confined destination path.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_upload(const application::FilesystemService &service, const HttpRequestView &request,
              std::string_view requested_path);

/// @brief Creates one empty directory at a confined request path.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_directory_creation(const application::FilesystemService &service,
                          const HttpRequestView &request, std::string_view requested_path);

/// @brief Deletes one file, link, or empty directory through the configured location policy.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_deletion(const application::FilesystemService &service, const HttpRequestView &request,
                std::string_view requested_path);

} // namespace sparenode::http::detail
