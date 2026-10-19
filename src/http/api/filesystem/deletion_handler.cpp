#include "handlers.hpp"

#include "filesystem_error_response.hpp"
#include "response_factory.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError> handle_deletion(const application::FilesystemService &service,
                                                     const HttpRequestView &request,
                                                     const std::string_view requested_path)
{
    if (auto removed = service.remove(requested_path, request.body_size()); !removed)
    {
        return make_public_error_response(removed.error());
    }
    return make_json_response(HttpStatusCode::ok, "{\"status\":\"deleted\"}");
}

} // namespace sparenode::http::detail
