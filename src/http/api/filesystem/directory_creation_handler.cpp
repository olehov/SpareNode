#include "handlers.hpp"

#include "filesystem_error_response.hpp"
#include "response_factory.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError>
handle_directory_creation(const application::FilesystemService &service,
                          const HttpRequestView &request, const std::string_view requested_path)
{
    if (auto created = service.create_directory(requested_path, request.body_size()); !created)
    {
        return make_public_error_response(created.error());
    }
    return make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
}

} // namespace sparenode::http::detail
