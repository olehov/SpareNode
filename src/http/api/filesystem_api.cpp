#include "sparenode/http/api/filesystem_api.hpp"

#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "filesystem/handlers.hpp"
#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/request/http_request.hpp"

namespace sparenode::http
{
namespace
{

using SharedLocation = std::shared_ptr<const FilesystemApiLocation>;
using SharedMimeTypes = std::shared_ptr<const MimeTypeRegistry>;

/// @brief Registers exact and nested protected variants of one location operation.
[[nodiscard]] Result<void, HttpRouteRegistrationError>
register_location_routes(api::ApiRouterBuilder &builder, const HttpMethod method,
                         const std::string_view api_path, HttpRouteHandler exact_handler,
                         HttpRouteHandler nested_handler)
{
    if (auto exact =
            builder.register_protected({method, std::string(api_path), std::move(exact_handler)});
        !exact)
    {
        return unexpected(exact.error());
    }
    const auto nested_path = api_path == "/" ? std::string("/*") : std::string(api_path) + "/*";
    return builder.register_protected({method, nested_path, std::move(nested_handler)});
}

/// @brief Converts one route-pair registration failure into an API construction failure.
[[nodiscard]] FilesystemApiError
registration_failure(const HttpRouteRegistrationError error) noexcept
{
    return {FilesystemApiErrorCode::route_registration_failed, error};
}

/// @brief Registers directory-listing and inline-download routes for one location.
[[nodiscard]] Result<void, HttpRouteRegistrationError>
register_read_routes(api::ApiRouterBuilder &builder, const SharedLocation &location,
                     const SharedMimeTypes &mime_types)
{
    return register_location_routes(
        builder, HttpMethod::get, location->api_path,
        [location, mime_types](const HttpRequestView &, const HttpRouteParameters &)
        { return detail::handle_resource(location->service, *mime_types, {}); },
        [location, mime_types](const HttpRequestView &, const HttpRouteParameters &parameters) {
            return detail::handle_resource(location->service, *mime_types,
                                           parameters.wildcard_suffix());
        });
}

/// @brief Registers upload routes for one location.
[[nodiscard]] Result<void, HttpRouteRegistrationError>
register_upload_routes(api::ApiRouterBuilder &builder, const SharedLocation &location)
{
    return register_location_routes(
        builder, HttpMethod::put, location->api_path,
        [location](const HttpRequestView &request, const HttpRouteParameters &)
        { return detail::handle_upload(location->service, request, {}); },
        [location](const HttpRequestView &request, const HttpRouteParameters &parameters) {
            return detail::handle_upload(location->service, request, parameters.wildcard_suffix());
        });
}

/// @brief Registers directory-creation routes for one location.
[[nodiscard]] Result<void, HttpRouteRegistrationError>
register_directory_creation_routes(api::ApiRouterBuilder &builder, const SharedLocation &location)
{
    return register_location_routes(
        builder, HttpMethod::post, location->api_path,
        [location](const HttpRequestView &request, const HttpRouteParameters &)
        { return detail::handle_directory_creation(location->service, request, {}); },
        [location](const HttpRequestView &request, const HttpRouteParameters &parameters)
        {
            return detail::handle_directory_creation(location->service, request,
                                                     parameters.wildcard_suffix());
        });
}

/// @brief Registers deletion routes for one location.
[[nodiscard]] Result<void, HttpRouteRegistrationError>
register_deletion_routes(api::ApiRouterBuilder &builder, const SharedLocation &location)
{
    return register_location_routes(
        builder, HttpMethod::delete_method, location->api_path,
        [location](const HttpRequestView &request, const HttpRouteParameters &)
        { return detail::handle_deletion(location->service, request, {}); },
        [location](const HttpRequestView &request, const HttpRouteParameters &parameters) {
            return detail::handle_deletion(location->service, request,
                                           parameters.wildcard_suffix());
        });
}

} // namespace

Result<HttpRouter, FilesystemApiError>
make_filesystem_api_router(FilesystemApiSettings settings,
                           api::ProtectedRouteDecorator protected_route_decorator)
{
    try
    {
        if (settings.locations.empty())
        {
            return unexpected(FilesystemApiError{FilesystemApiErrorCode::missing_location, {}});
        }

        const auto mime_types =
            std::make_shared<const MimeTypeRegistry>(std::move(settings.mime_types));
        api::ApiRouterBuilder builder(std::move(protected_route_decorator));
        for (auto &location : settings.locations)
        {
            const auto shared_location =
                std::make_shared<const FilesystemApiLocation>(std::move(location));
            if (auto read = register_read_routes(builder, shared_location, mime_types); !read)
            {
                return unexpected(registration_failure(read.error()));
            }
            if (auto upload = register_upload_routes(builder, shared_location); !upload)
            {
                return unexpected(registration_failure(upload.error()));
            }
            if (auto create = register_directory_creation_routes(builder, shared_location); !create)
            {
                return unexpected(registration_failure(create.error()));
            }
            if (auto remove = register_deletion_routes(builder, shared_location); !remove)
            {
                return unexpected(registration_failure(remove.error()));
            }
        }
        return std::move(builder).build();
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            FilesystemApiError{FilesystemApiErrorCode::resource_allocation_failed, {}});
    }
}

const char *to_string(const FilesystemApiErrorCode code) noexcept
{
    switch (code)
    {
    case FilesystemApiErrorCode::missing_location:
        return "filesystem API requires a configured location";
    case FilesystemApiErrorCode::route_registration_failed:
        return "filesystem API route registration failed";
    case FilesystemApiErrorCode::resource_allocation_failed:
        return "filesystem API memory allocation failed";
    }
    return "unknown filesystem API error";
}

} // namespace sparenode::http
