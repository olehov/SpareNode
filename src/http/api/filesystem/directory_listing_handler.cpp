#include "handlers.hpp"

#include <new>

#include "filesystem_error_response.hpp"
#include "json_writer.hpp"
#include "response_factory.hpp"
#include "sparenode/filesystem/directory_listing.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http::detail
{

Result<HttpResponse, HttpRouteError> handle_resource(const FilesystemApiLocation &location,
                                                     const MimeTypeRegistry &mime_types,
                                                     const std::string_view requested_path)
{
    try
    {
        if (!location.permissions.allows_read())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"read_forbidden\"}");
        }
        const auto listing = filesystem::list_directory(location.root, requested_path);
        if (!listing)
        {
            if (listing.error().code == filesystem::DirectoryListingErrorCode::not_directory)
            {
                return handle_file(location, mime_types, requested_path);
            }
            return make_public_error_response(listing.error());
        }
        return make_json_response(HttpStatusCode::ok, serialize_listing(listing.value()));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(filesystem::DirectoryListingErrorCode::resource_allocation_failed)});
    }
}

} // namespace sparenode::http::detail
