#pragma once

#include "sparenode/filesystem/directory_creator.hpp"
#include "sparenode/filesystem/directory_listing.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"
#include "sparenode/filesystem/filesystem_entry_remover.hpp"
#include "sparenode/filesystem/upload_finalizer.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/http/routing/http_router.hpp"
#include "sparenode/storage/temporary_file.hpp"

namespace sparenode::http::detail
{

/// @brief Converts one directory-listing failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const filesystem::DirectoryListingError &error);

/// @brief Converts one file-read failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const filesystem::FileReadError &error);

/// @brief Converts one upload-publication failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const filesystem::UploadFinalizationError &error);

/// @brief Converts one temporary-file failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const storage::TemporaryFileError &error);

/// @brief Converts one directory-creation failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const filesystem::DirectoryCreationError &error);

/// @brief Converts one filesystem-removal failure into a stable public response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const filesystem::FilesystemEntryRemovalError &error);

} // namespace sparenode::http::detail
