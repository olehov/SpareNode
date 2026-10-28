#include "handlers.hpp"

#include <new>

#include "filesystem_error_response.hpp"
#include "sparenode/http/api/json_response.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

/// @brief Adapts one decoded HTTP request body to the filesystem upload use case.
Result<HttpResponse, HttpRouteError> handle_upload(const application::FilesystemService &service,
                                                   const HttpRequestView &request,
                                                   const std::string_view requested_path)
{
    try
    {
        const application::FilesystemUploadContent content{request.body(), request.body_size(),
                                                           request.temporary_body(),
                                                           request.temporary_file_options()};
        if (auto uploaded = service.upload(requested_path, content); !uploaded)
        {
            return make_public_error_response(uploaded.error());
        }
        return api::make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(application::FilesystemUploadErrorCode::resource_allocation_failed)});
    }
}

} // namespace sparenode::http::detail
