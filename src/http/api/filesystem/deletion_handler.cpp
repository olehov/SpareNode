#include "handlers.hpp"

#include <new>

#include "filesystem_error_response.hpp"
#include "sparenode/http/api/json_response.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

/// @brief Adapts one HTTP request to the confined filesystem-removal use case.
Result<HttpResponse, HttpRouteError> handle_deletion(const application::FilesystemService &service,
                                                     const HttpRequestView &request,
                                                     const std::string_view requested_path)
{
    try
    {
        const auto file_options = request.temporary_file_options();
        if (auto removed = service.remove(requested_path, request.body_size(),
                                          {file_options.stop_token, file_options.deadline});
            !removed)
        {
            return make_public_error_response(removed.error());
        }
        return api::make_json_response(HttpStatusCode::ok, "{\"status\":\"deleted\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(application::FilesystemRemovalErrorCode::resource_allocation_failed)});
    }
}

} // namespace sparenode::http::detail
