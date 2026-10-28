#include "filesystem_error_response.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "json_writer.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/http/api/json_response.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{
namespace
{

using PublicFailure = std::pair<HttpStatusCode, std::string_view>;

/// @brief Reports whether a path rejection must appear publicly as an absent resource.
[[nodiscard]] bool hides_path_detail(const filesystem::SafePathErrorCode code) noexcept
{
    return code == filesystem::SafePathErrorCode::outside_shared_root ||
           code == filesystem::SafePathErrorCode::resolution_failed ||
           code == filesystem::SafePathErrorCode::unsupported_reparse_point;
}

/// @brief Maps one directory-listing failure to its safe HTTP representation.
[[nodiscard]] PublicFailure public_error(const filesystem::DirectoryListingError &error) noexcept
{
    using enum filesystem::DirectoryListingErrorCode;
    switch (error.code)
    {
    case invalid_path:
        return error.path_error && hides_path_detail(*error.path_error)
                   ? PublicFailure{HttpStatusCode::not_found, "not_found"}
                   : PublicFailure{HttpStatusCode::bad_request, "invalid_path"};
    case not_found:
        return {HttpStatusCode::not_found, "not_found"};
    case not_directory:
        return {HttpStatusCode::bad_request, "not_directory"};
    case permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case too_many_entries:
        return {HttpStatusCode::content_too_large, "directory_too_large"};
    case filesystem_failure:
    case resource_allocation_failed:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one file-open failure to its safe HTTP representation.
[[nodiscard]] PublicFailure public_error(const filesystem::FileReadError &error) noexcept
{
    using enum filesystem::FileReadErrorCode;
    switch (error.code)
    {
    case invalid_path:
        return error.path_error && hides_path_detail(*error.path_error)
                   ? PublicFailure{HttpStatusCode::not_found, "not_found"}
                   : PublicFailure{HttpStatusCode::bad_request, "invalid_path"};
    case not_found:
    case not_regular_file:
    case outside_shared_root:
        return {HttpStatusCode::not_found, "not_found"};
    case permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case filesystem_failure:
    case cancelled:
    case resource_allocation_failed:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one upload-publication failure to its safe HTTP representation.
[[nodiscard]] PublicFailure public_error(const filesystem::UploadFinalizationError &error) noexcept
{
    using enum filesystem::UploadFinalizationErrorCode;
    switch (error.code)
    {
    case invalid_destination:
        return error.path_error && hides_path_detail(error.path_error->code)
                   ? PublicFailure{HttpStatusCode::not_found, "not_found"}
                   : PublicFailure{HttpStatusCode::bad_request, "invalid_path"};
    case parent_unavailable:
        return {HttpStatusCode::not_found, "parent_not_found"};
    case destination_exists:
        return {HttpStatusCode::conflict, "destination_exists"};
    case invalid_source:
        return {HttpStatusCode::bad_request, "invalid_upload"};
    case cancelled:
    case deadline_exceeded:
        return {HttpStatusCode::request_timeout, "upload_interrupted"};
    case source_read_failed:
    case staging_failed:
    case staging_write_failed:
    case staging_flush_failed:
    case publish_failed:
    case resource_failure:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one temporary-storage failure to its safe HTTP representation.
[[nodiscard]] PublicFailure public_error(const storage::TemporaryFileError &error) noexcept
{
    using enum storage::TemporaryFileErrorCode;
    switch (error.code)
    {
    case cancelled:
    case deadline_exceeded:
        return {HttpStatusCode::request_timeout, "upload_interrupted"};
    case temporary_directory_unavailable:
    case create_failed:
    case write_failed:
    case flush_failed:
    case close_failed:
    case invalid_state:
    case resource_allocation_failed:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one directory-creation failure to its safe HTTP representation.
[[nodiscard]] PublicFailure public_error(const filesystem::DirectoryCreationError &error) noexcept
{
    using enum filesystem::DirectoryCreationErrorCode;
    switch (error.code)
    {
    case invalid_destination:
        return error.path_error && hides_path_detail(error.path_error->code)
                   ? PublicFailure{HttpStatusCode::not_found, "not_found"}
                   : PublicFailure{HttpStatusCode::bad_request, "invalid_path"};
    case parent_unavailable:
        return {HttpStatusCode::not_found, "parent_not_found"};
    case destination_exists:
        return {HttpStatusCode::conflict, "destination_exists"};
    case permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case creation_failed:
    case resource_failure:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one confined-removal failure to its safe HTTP representation.
[[nodiscard]] PublicFailure
public_error(const filesystem::FilesystemEntryRemovalError &error) noexcept
{
    using enum filesystem::FilesystemEntryRemovalErrorCode;
    switch (error.code)
    {
    case invalid_destination:
        return error.path_error && hides_path_detail(error.path_error->code)
                   ? PublicFailure{HttpStatusCode::not_found, "not_found"}
                   : PublicFailure{HttpStatusCode::bad_request, "invalid_path"};
    case parent_unavailable:
    case not_found:
        return {HttpStatusCode::not_found, "not_found"};
    case directory_not_empty:
        return {HttpStatusCode::conflict, "directory_not_empty"};
    case permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case removal_failed:
    case resource_failure:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Serializes one public failure identifier into a JSON HTTP response.
[[nodiscard]] Result<HttpResponse, HttpRouteError> make_response(const PublicFailure public_failure)
{
    const auto [status, identifier] = public_failure;
    std::string body("{\"error\":");
    append_json_string(body, identifier);
    body.push_back('}');
    return api::make_json_response(status, body);
}

} // namespace

/// @brief Converts a filesystem-read application failure into a public response.
Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemReadError &error)
{
    using enum application::FilesystemReadErrorCode;
    switch (error.code)
    {
    case forbidden:
        return make_response({HttpStatusCode::forbidden, "read_forbidden"});
    case directory_listing_failed:
        return make_response(
            public_error(std::get<filesystem::DirectoryListingError>(error.detail)));
    case file_read_failed:
        return make_response(public_error(std::get<filesystem::FileReadError>(error.detail)));
    case resource_allocation_failed:
        return make_response({HttpStatusCode::internal_server_error, "internal_error"});
    }
    return make_response({HttpStatusCode::internal_server_error, "internal_error"});
}

/// @brief Converts a filesystem-upload application failure into a public response.
Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemUploadError &error)
{
    using enum application::FilesystemUploadErrorCode;
    switch (error.code)
    {
    case forbidden:
        return make_response({HttpStatusCode::forbidden, "write_forbidden"});
    case invalid_upload:
        return make_response({HttpStatusCode::bad_request, "invalid_upload"});
    case temporary_file_failed:
        return make_response(public_error(std::get<storage::TemporaryFileError>(error.detail)));
    case upload_finalization_failed:
        return make_response(
            public_error(std::get<filesystem::UploadFinalizationError>(error.detail)));
    case resource_allocation_failed:
        return make_response({HttpStatusCode::internal_server_error, "internal_error"});
    }
    return make_response({HttpStatusCode::internal_server_error, "internal_error"});
}

/// @brief Converts a directory-creation application failure into a public response.
Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemDirectoryCreationError &error)
{
    using enum application::FilesystemDirectoryCreationErrorCode;
    switch (error.code)
    {
    case forbidden:
        return make_response({HttpStatusCode::forbidden, "write_forbidden"});
    case content_not_allowed:
        return make_response({HttpStatusCode::bad_request, "body_not_allowed"});
    case directory_creation_failed:
        return make_response(
            public_error(std::get<filesystem::DirectoryCreationError>(error.detail)));
    case resource_allocation_failed:
        return make_response({HttpStatusCode::internal_server_error, "internal_error"});
    }
    return make_response({HttpStatusCode::internal_server_error, "internal_error"});
}

/// @brief Converts a filesystem-removal application failure into a public response.
Result<HttpResponse, HttpRouteError>
make_public_error_response(const application::FilesystemRemovalError &error)
{
    using enum application::FilesystemRemovalErrorCode;
    switch (error.code)
    {
    case forbidden:
        return make_response({HttpStatusCode::forbidden, "delete_forbidden"});
    case content_not_allowed:
        return make_response({HttpStatusCode::bad_request, "body_not_allowed"});
    case filesystem_removal_failed:
        return make_response(
            public_error(std::get<filesystem::FilesystemEntryRemovalError>(error.detail)));
    case resource_allocation_failed:
        return make_response({HttpStatusCode::internal_server_error, "internal_error"});
    }
    return make_response({HttpStatusCode::internal_server_error, "internal_error"});
}

} // namespace sparenode::http::detail
