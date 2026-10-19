#include "handlers.hpp"

#include "filesystem_error_response.hpp"
#include "response_factory.hpp"
#include "sparenode/http/http_status_code.hpp"
#include <new>

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError> handle_upload(const application::FilesystemService &service,
                                                   const HttpRequestView &request,
                                                   const std::string_view requested_path)
{
    const application::FilesystemUploadContent content{request.body(), request.body_size(),
                                                       request.temporary_body(),
                                                       request.temporary_file_options()};
    if (auto uploaded = service.upload(requested_path, content); !uploaded)
    {
        return make_public_error_response(uploaded.error());
    }
    return make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
}

} // namespace sparenode::http::detail
