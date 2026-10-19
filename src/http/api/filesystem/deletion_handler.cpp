#include "handlers.hpp"

#include <new>

#include "filesystem_error_response.hpp"
#include "response_factory.hpp"
#include "sparenode/filesystem/filesystem_entry_remover.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError> handle_deletion(const FilesystemApiLocation &location,
                                                     const HttpRequestView &request,
                                                     const std::string_view requested_path)
{
    try
    {
        if (!location.permissions.allows_delete())
        {
            return make_json_response(HttpStatusCode::forbidden,
                                      "{\"error\":\"delete_forbidden\"}");
        }
        if (request.body_size() != 0)
        {
            return make_json_response(HttpStatusCode::bad_request,
                                      "{\"error\":\"body_not_allowed\"}");
        }
        if (auto removed = filesystem::remove_filesystem_entry(location.root, requested_path);
            !removed)
        {
            return make_public_error_response(removed.error());
        }
        return make_json_response(HttpStatusCode::ok, "{\"status\":\"deleted\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(filesystem::FilesystemEntryRemovalErrorCode::resource_failure)});
    }
}

} // namespace sparenode::http::detail
