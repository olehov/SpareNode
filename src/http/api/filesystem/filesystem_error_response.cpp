#include "filesystem_error_response.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "json_writer.hpp"
#include "response_factory.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{
namespace
{

using PublicFailure = std::pair<HttpStatusCode, std::string_view>;

[[nodiscard]] bool hides_path_detail(const filesystem::SafePathErrorCode code) noexcept
{
    return code == filesystem::SafePathErrorCode::outside_shared_root ||
           code == filesystem::SafePathErrorCode::resolution_failed ||
           code == filesystem::SafePathErrorCode::unsupported_reparse_point;
}

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

[[nodiscard]] Result<HttpResponse, HttpRouteError> make_response(const PublicFailure public_failure)
{
    const auto [status, identifier] = public_failure;
    std::string body("{\"error\":");
    append_json_string(body, identifier);
    body.push_back('}');
    return make_json_response(status, body);
}

} // namespace

#define SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(ErrorType)                                          \
    Result<HttpResponse, HttpRouteError> make_public_error_response(const ErrorType &error)        \
    {                                                                                              \
        return make_response(public_error(error));                                                 \
    }

SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(filesystem::DirectoryListingError)
SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(filesystem::FileReadError)
SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(filesystem::UploadFinalizationError)
SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(storage::TemporaryFileError)
SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(filesystem::DirectoryCreationError)
SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE(filesystem::FilesystemEntryRemovalError)

#undef SPARENODE_DEFINE_PUBLIC_ERROR_RESPONSE

} // namespace sparenode::http::detail
