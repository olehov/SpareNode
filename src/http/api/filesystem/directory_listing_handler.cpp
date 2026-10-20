#include "handlers.hpp"

#include <new>

#include "filesystem_error_response.hpp"
#include "json_writer.hpp"
#include "response_factory.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError> handle_resource(const application::FilesystemService &service,
                                                     const MimeTypeRegistry &mime_types,
                                                     const std::string_view requested_path)
{
    try
    {
        auto resource = service.read(requested_path);
        if (!resource)
        {
            return make_public_error_response(resource.error());
        }
        auto resolved = std::move(resource).value();
        if (const auto *listing =
                std::get_if<std::vector<filesystem::DirectoryListingEntry>>(&resolved))
        {
            return make_json_response(HttpStatusCode::ok, serialize_listing(*listing));
        }
        return handle_file(std::get<filesystem::FileReadStream>(std::move(resolved)), mime_types,
                           requested_path);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(application::FilesystemReadErrorCode::resource_allocation_failed)});
    }
}

} // namespace sparenode::http::detail
