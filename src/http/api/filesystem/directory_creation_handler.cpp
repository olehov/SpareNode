#include "handlers.hpp"

#include <new>

#include "filesystem_error_response.hpp"
#include "response_factory.hpp"
#include "sparenode/filesystem/directory_creator.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError>
handle_directory_creation(const FilesystemApiLocation &location, const HttpRequestView &request,
                          const std::string_view requested_path)
{
    try
    {
        if (!location.permissions.allows_write())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"write_forbidden\"}");
        }
        if (request.body_size() != 0)
        {
            return make_json_response(HttpStatusCode::bad_request,
                                      "{\"error\":\"body_not_allowed\"}");
        }
        const auto created = filesystem::create_directory(location.root, requested_path);
        if (!created)
        {
            return make_public_error_response(created.error());
        }
        return make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(filesystem::DirectoryCreationErrorCode::resource_failure)});
    }
}

} // namespace sparenode::http::detail
