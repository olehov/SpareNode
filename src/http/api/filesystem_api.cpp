#include "sparenode/http/api/filesystem_api.hpp"

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/configuration/runtime/location_config.hpp"
#include "sparenode/filesystem/detail/path_request_decoder.hpp"
#include "sparenode/filesystem/directory_creator.hpp"
#include "sparenode/filesystem/directory_listing.hpp"
#include "sparenode/filesystem/file_read_stream.hpp"
#include "sparenode/filesystem/filesystem_entry_remover.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/filesystem/temporary_file.hpp"
#include "sparenode/filesystem/upload_finalizer.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/mime_type_registry.hpp"
#include "sparenode/http/request/http_request.hpp"
#include "sparenode/http/response/http_response.hpp"

namespace sparenode::http
{
namespace
{

using configuration::runtime::LocationConfig;
using filesystem::DirectoryCreationError;
using filesystem::DirectoryCreationErrorCode;
using filesystem::DirectoryListingEntry;
using filesystem::DirectoryListingError;
using filesystem::DirectoryListingErrorCode;
using filesystem::FileReadError;
using filesystem::FileReadErrorCode;
using filesystem::FilesystemEntryRemovalError;
using filesystem::FilesystemEntryRemovalErrorCode;
using filesystem::TemporaryFileError;
using filesystem::TemporaryFileErrorCode;
using filesystem::UploadFinalizationError;
using filesystem::UploadFinalizationErrorCode;

/// @brief Appends one string using JSON escaping without changing valid UTF-8 bytes.
void append_json_string(std::string &output, const std::string_view value)
{
    constexpr std::string_view hexadecimal = "0123456789ABCDEF";
    output.push_back('"');
    for (const unsigned char byte : value)
    {
        switch (byte)
        {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            // JSON requires every control byte from 0x00 through 0x1F to be escaped.
            if (byte < 0x20U)
            {
                output.append("\\u00");

                // Convert the byte's high four bits into the first hexadecimal digit.
                output.push_back(hexadecimal[byte >> 4U]);

                // Mask the low four bits and convert them into the second hexadecimal digit.
                output.push_back(hexadecimal[byte & 0x0FU]);
            }
            else
            {
                output.push_back(static_cast<char>(byte));
            }
        }
    }
    output.push_back('"');
}

/// @brief Formats one second-resolution system time as RFC 3339 UTC.
[[nodiscard]] std::string format_timestamp(const std::chrono::sys_seconds value)
{
    const auto day_point = std::chrono::floor<std::chrono::days>(value);
    const std::chrono::year_month_day date(day_point);
    const std::chrono::hh_mm_ss time(value - day_point);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-'
           << std::setw(2) << static_cast<unsigned int>(date.month()) << '-' << std::setw(2)
           << static_cast<unsigned int>(date.day()) << 'T' << std::setw(2) << time.hours().count()
           << ':' << std::setw(2) << time.minutes().count() << ':' << std::setw(2)
           << time.seconds().count() << 'Z';
    return output.str();
}

/// @brief Returns the JSON label for one portable entry category.
[[nodiscard]] constexpr std::string_view
entry_type_name(const filesystem::DirectoryEntryType type) noexcept
{
    switch (type)
    {
    case filesystem::DirectoryEntryType::regular_file:
        return "file";
    case filesystem::DirectoryEntryType::directory:
        return "directory";
    case filesystem::DirectoryEntryType::symbolic_link:
        return "symlink";
    case filesystem::DirectoryEntryType::other:
        return "other";
    }
    return "other";
}

/// @brief Serializes a bounded name-sorted listing without exposing native paths.
[[nodiscard]] std::string serialize_listing(const std::vector<DirectoryListingEntry> &entries)
{
    constexpr std::size_t outer_object_estimate = 16;
    constexpr std::size_t typical_entry_estimate = 96;
    std::string output;
    // Reserve a small outer JSON object plus typical serialized metadata for each entry.
    // This only reduces reallocations; the string still grows when names require more space.
    output.reserve(outer_object_estimate + entries.size() * typical_entry_estimate);
    output.append("{\"entries\":[");
    bool first = true;
    for (const auto &entry : entries)
    {
        if (!first)
        {
            output.push_back(',');
        }
        first = false;
        output.append("{\"name\":");
        append_json_string(output, entry.name);
        output.append(",\"type\":");
        append_json_string(output, entry_type_name(entry.type));
        output.append(",\"size\":");
        if (entry.size)
        {
            output.append(std::to_string(entry.size.value()));
        }
        else
        {
            output.append("null");
        }
        output.append(",\"modifiedAt\":");
        append_json_string(output, format_timestamp(entry.modification_time));
        output.push_back('}');
    }
    output.append("]}");
    return output;
}

/// @brief Returns the standard reason phrase used by this API.
[[nodiscard]] constexpr std::string_view reason_phrase(const HttpStatusCode status) noexcept
{
    switch (status)
    {
    case HttpStatusCode::ok:
        return "OK";
    case HttpStatusCode::created:
        return "Created";
    case HttpStatusCode::bad_request:
        return "Bad Request";
    case HttpStatusCode::forbidden:
        return "Forbidden";
    case HttpStatusCode::not_found:
        return "Not Found";
    case HttpStatusCode::request_timeout:
        return "Request Timeout";
    case HttpStatusCode::conflict:
        return "Conflict";
    case HttpStatusCode::content_too_large:
        return "Content Too Large";
    default:
        return "Internal Server Error";
    }
}

/// @brief Creates a validated JSON response for a route handler.
[[nodiscard]] Result<HttpResponse, HttpRouteError> make_json_response(const HttpStatusCode status,
                                                                      const std::string &body)
{
    const auto bytes = std::as_bytes(std::span(body.data(), body.size()));
    std::vector<std::byte> owned_body(bytes.begin(), bytes.end());
    auto response = HttpResponse::create(status, std::string(reason_phrase(status)),
                                         {{"Content-Type", "application/json; charset=utf-8"}},
                                         std::move(owned_body));
    if (!response)
    {
        return unexpected(HttpRouteError{HttpRouteErrorCode::response_validation_failure,
                                         static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

/// @brief Maps one listing failure to a public status and stable error identifier.
[[nodiscard]] std::pair<HttpStatusCode, std::string_view>
public_error(const DirectoryListingError &error) noexcept
{
    switch (error.code)
    {
    case DirectoryListingErrorCode::invalid_path:
        if (error.path_error == filesystem::SafePathErrorCode::outside_shared_root ||
            error.path_error == filesystem::SafePathErrorCode::resolution_failed ||
            error.path_error == filesystem::SafePathErrorCode::unsupported_reparse_point)
        {
            return {HttpStatusCode::not_found, "not_found"};
        }
        return {HttpStatusCode::bad_request, "invalid_path"};
    case DirectoryListingErrorCode::not_found:
        return {HttpStatusCode::not_found, "not_found"};
    case DirectoryListingErrorCode::not_directory:
        return {HttpStatusCode::bad_request, "not_directory"};
    case DirectoryListingErrorCode::permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case DirectoryListingErrorCode::too_many_entries:
        return {HttpStatusCode::content_too_large, "directory_too_large"};
    case DirectoryListingErrorCode::filesystem_failure:
    case DirectoryListingErrorCode::resource_allocation_failed:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one file-open failure to a public status and stable error identifier.
[[nodiscard]] std::pair<HttpStatusCode, std::string_view>
public_error(const FileReadError &error) noexcept
{
    switch (error.code)
    {
    case FileReadErrorCode::invalid_path:
        if (error.path_error == filesystem::SafePathErrorCode::outside_shared_root ||
            error.path_error == filesystem::SafePathErrorCode::resolution_failed ||
            error.path_error == filesystem::SafePathErrorCode::unsupported_reparse_point)
        {
            return {HttpStatusCode::not_found, "not_found"};
        }
        return {HttpStatusCode::bad_request, "invalid_path"};
    case FileReadErrorCode::not_found:
    case FileReadErrorCode::not_regular_file:
    case FileReadErrorCode::outside_shared_root:
        return {HttpStatusCode::not_found, "not_found"};
    case FileReadErrorCode::permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case FileReadErrorCode::filesystem_failure:
    case FileReadErrorCode::cancelled:
    case FileReadErrorCode::resource_allocation_failed:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one upload publication failure without exposing native paths or error details.
[[nodiscard]] std::pair<HttpStatusCode, std::string_view>
public_error(const UploadFinalizationError &error) noexcept
{
    switch (error.code)
    {
    case UploadFinalizationErrorCode::invalid_destination:
        if (error.path_error.has_value() &&
            (error.path_error->code == filesystem::SafePathErrorCode::outside_shared_root ||
             error.path_error->code == filesystem::SafePathErrorCode::resolution_failed ||
             error.path_error->code == filesystem::SafePathErrorCode::unsupported_reparse_point))
        {
            return {HttpStatusCode::not_found, "not_found"};
        }
        return {HttpStatusCode::bad_request, "invalid_path"};
    case UploadFinalizationErrorCode::parent_unavailable:
        return {HttpStatusCode::not_found, "parent_not_found"};
    case UploadFinalizationErrorCode::destination_exists:
        return {HttpStatusCode::conflict, "destination_exists"};
    case UploadFinalizationErrorCode::invalid_source:
        return {HttpStatusCode::bad_request, "invalid_upload"};
    case UploadFinalizationErrorCode::cancelled:
    case UploadFinalizationErrorCode::deadline_exceeded:
        return {HttpStatusCode::request_timeout, "upload_interrupted"};
    case UploadFinalizationErrorCode::source_read_failed:
    case UploadFinalizationErrorCode::staging_failed:
    case UploadFinalizationErrorCode::staging_write_failed:
    case UploadFinalizationErrorCode::staging_flush_failed:
    case UploadFinalizationErrorCode::publish_failed:
    case UploadFinalizationErrorCode::resource_failure:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps temporary-source lifecycle failures to stable upload responses.
[[nodiscard]] std::pair<HttpStatusCode, std::string_view>
public_error(const TemporaryFileError &error) noexcept
{
    switch (error.code)
    {
    case TemporaryFileErrorCode::cancelled:
    case TemporaryFileErrorCode::deadline_exceeded:
        return {HttpStatusCode::request_timeout, "upload_interrupted"};
    case TemporaryFileErrorCode::temporary_directory_unavailable:
    case TemporaryFileErrorCode::create_failed:
    case TemporaryFileErrorCode::write_failed:
    case TemporaryFileErrorCode::flush_failed:
    case TemporaryFileErrorCode::close_failed:
    case TemporaryFileErrorCode::invalid_state:
    case TemporaryFileErrorCode::resource_allocation_failed:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one directory-creation failure without exposing native paths or errors.
[[nodiscard]] std::pair<HttpStatusCode, std::string_view>
public_error(const DirectoryCreationError &error) noexcept
{
    switch (error.code)
    {
    case DirectoryCreationErrorCode::invalid_destination:
        if (error.path_error.has_value() &&
            (error.path_error->code == filesystem::SafePathErrorCode::outside_shared_root ||
             error.path_error->code == filesystem::SafePathErrorCode::resolution_failed ||
             error.path_error->code == filesystem::SafePathErrorCode::unsupported_reparse_point))
        {
            return {HttpStatusCode::not_found, "not_found"};
        }
        return {HttpStatusCode::bad_request, "invalid_path"};
    case DirectoryCreationErrorCode::parent_unavailable:
        return {HttpStatusCode::not_found, "parent_not_found"};
    case DirectoryCreationErrorCode::destination_exists:
        return {HttpStatusCode::conflict, "destination_exists"};
    case DirectoryCreationErrorCode::permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case DirectoryCreationErrorCode::creation_failed:
    case DirectoryCreationErrorCode::resource_failure:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Maps one deletion failure without exposing native paths or errors.
[[nodiscard]] std::pair<HttpStatusCode, std::string_view>
public_error(const FilesystemEntryRemovalError &error) noexcept
{
    switch (error.code)
    {
    case FilesystemEntryRemovalErrorCode::invalid_destination:
        if (error.path_error.has_value() &&
            (error.path_error->code == filesystem::SafePathErrorCode::outside_shared_root ||
             error.path_error->code == filesystem::SafePathErrorCode::resolution_failed ||
             error.path_error->code == filesystem::SafePathErrorCode::unsupported_reparse_point))
        {
            return {HttpStatusCode::not_found, "not_found"};
        }
        return {HttpStatusCode::bad_request, "invalid_path"};
    case FilesystemEntryRemovalErrorCode::parent_unavailable:
    case FilesystemEntryRemovalErrorCode::not_found:
        return {HttpStatusCode::not_found, "not_found"};
    case FilesystemEntryRemovalErrorCode::directory_not_empty:
        return {HttpStatusCode::conflict, "directory_not_empty"};
    case FilesystemEntryRemovalErrorCode::permission_denied:
        return {HttpStatusCode::forbidden, "permission_denied"};
    case FilesystemEntryRemovalErrorCode::removal_failed:
    case FilesystemEntryRemovalErrorCode::resource_failure:
        return {HttpStatusCode::internal_server_error, "internal_error"};
    }
    return {HttpStatusCode::internal_server_error, "internal_error"};
}

/// @brief Converts a confined file stream into a fixed-length HTTP body source.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_file_response(filesystem::FileReadStream stream, const std::string_view content_type)
{
    const auto content_length = stream.size();
    HttpBodyReader reader = [stream = std::move(stream)](const std::span<std::byte> destination,
                                                         const std::stop_token &stop_token) mutable
        -> Result<std::size_t, HttpBodyReadError>
    {
        auto chunk = stream.read(destination, stop_token);
        if (chunk)
        {
            return chunk.value();
        }
        const auto &error = chunk.error();
        const auto domain = error.code == FileReadErrorCode::cancelled
                                ? HttpBodyReadErrorDomain::cancellation
                                : HttpBodyReadErrorDomain::filesystem;
        return unexpected(HttpBodyReadError{domain, static_cast<int>(error.code)});
    };
    auto response = HttpResponse::create_streaming(HttpStatusCode::ok, "OK",
                                                   {{"Content-Type", std::string(content_type)},
                                                    {"Content-Disposition", "inline"},
                                                    {"X-Content-Type-Options", "nosniff"}},
                                                   content_length, std::move(reader));
    if (!response)
    {
        return unexpected(HttpRouteError{HttpRouteErrorCode::response_validation_failure,
                                         static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

/// @brief Creates a JSON response from one public status and error identifier.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_public_error_response(const std::pair<HttpStatusCode, std::string_view> public_failure)
{
    const auto [status, identifier] = public_failure;
    std::string body("{\"error\":");
    append_json_string(body, identifier);
    body.push_back('}');
    return make_json_response(status, body);
}

/// @brief Opens one file request and creates its streaming response.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_file(const LocationConfig &location, const MimeTypeRegistry &mime_types,
            const std::string_view requested_path)
{
    auto file = filesystem::FileReadStream::open(location.root(), requested_path);
    if (!file)
    {
        return make_public_error_response(public_error(file.error()));
    }
    const auto decoded_path = filesystem::detail::decode_path_request(requested_path);
    const auto content_type = decoded_path ? mime_types.content_type_for_path(decoded_path.value())
                                           : mime_types.content_type_for_path({});
    return make_file_response(std::move(file).value(), content_type);
}

/// @brief Handles one exact or wildcard directory-listing or inline-file route.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_resource(const LocationConfig &location, const MimeTypeRegistry &mime_types,
                const std::string_view requested_path)
{
    try
    {
        if (!location.permissions().allows_read())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"read_forbidden\"}");
        }
        const auto listing = filesystem::list_directory(location.root(), requested_path);
        if (!listing)
        {
            if (listing.error().code == DirectoryListingErrorCode::not_directory)
            {
                return handle_file(location, mime_types, requested_path);
            }
            return make_public_error_response(public_error(listing.error()));
        }
        return make_json_response(HttpStatusCode::ok, serialize_listing(listing.value()));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(DirectoryListingErrorCode::resource_allocation_failed)});
    }
}

/// @brief Publishes one complete decoded request body under a confined destination path.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_upload(const LocationConfig &location, const HttpRequestView &request,
              const std::string_view requested_path)
{
    try
    {
        if (!location.permissions().allows_write())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"write_forbidden\"}");
        }

        auto retained_artifact = request.temporary_body();
        std::optional<filesystem::TemporaryFile> materialized_artifact;
        const filesystem::TemporaryFile *source = retained_artifact.get();
        if (source == nullptr)
        {
            if (request.body_size() != request.body().size())
            {
                return make_json_response(HttpStatusCode::bad_request,
                                          "{\"error\":\"invalid_upload\"}");
            }
            auto created = filesystem::TemporaryFile::create();
            if (!created)
            {
                return make_public_error_response(public_error(created.error()));
            }
            materialized_artifact.emplace(std::move(created).value());
            const auto options = request.temporary_file_options();
            if (auto written = materialized_artifact->write(request.body(), options); !written)
            {
                return make_public_error_response(public_error(written.error()));
            }
            if (auto completed = materialized_artifact->complete(options); !completed)
            {
                return make_public_error_response(public_error(completed.error()));
            }
            source = &materialized_artifact.value();
        }

        auto published = filesystem::finalize_upload(location.root(), requested_path, *source,
                                                     request.temporary_file_options());
        if (!published)
        {
            return make_public_error_response(public_error(published.error()));
        }
        return make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            HttpRouteError{HttpRouteErrorCode::handler_failure,
                           static_cast<int>(UploadFinalizationErrorCode::resource_failure)});
    }
}

/// @brief Creates one empty directory at a confined request path.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_directory_creation(const LocationConfig &location, const HttpRequestView &request,
                          const std::string_view requested_path)
{
    try
    {
        if (!location.permissions().allows_write())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"write_forbidden\"}");
        }
        if (request.body_size() != 0)
        {
            return make_json_response(HttpStatusCode::bad_request,
                                      "{\"error\":\"body_not_allowed\"}");
        }
        const auto created = filesystem::create_directory(location.root(), requested_path);
        if (!created)
        {
            return make_public_error_response(public_error(created.error()));
        }
        return make_json_response(HttpStatusCode::created, "{\"status\":\"created\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            HttpRouteError{HttpRouteErrorCode::handler_failure,
                           static_cast<int>(DirectoryCreationErrorCode::resource_failure)});
    }
}

/// @brief Deletes one file, link, or empty directory through the configured location policy.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_deletion(const LocationConfig &location, const HttpRequestView &request,
                const std::string_view requested_path)
{
    try
    {
        if (!location.permissions().allows_delete())
        {
            return make_json_response(HttpStatusCode::forbidden,
                                      "{\"error\":\"delete_forbidden\"}");
        }
        if (request.body_size() != 0)
        {
            return make_json_response(HttpStatusCode::bad_request,
                                      "{\"error\":\"body_not_allowed\"}");
        }
        if (auto removed = filesystem::remove_filesystem_entry(location.root(), requested_path);
            !removed)
        {
            return make_public_error_response(public_error(removed.error()));
        }
        return make_json_response(HttpStatusCode::ok, "{\"status\":\"deleted\"}");
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            HttpRouteError{HttpRouteErrorCode::handler_failure,
                           static_cast<int>(FilesystemEntryRemovalErrorCode::resource_failure)});
    }
}

/// @brief Registers exact and nested variants of one location operation.
[[nodiscard]] Result<void, HttpRouteRegistrationError>
register_location_routes(HttpRouter &router, const HttpMethod method,
                         const std::string_view api_path, HttpRouteHandler exact_handler,
                         HttpRouteHandler nested_handler)
{
    if (auto exact = router.register_route(method, std::string(api_path), std::move(exact_handler));
        !exact)
    {
        return unexpected(exact.error());
    }
    const auto nested_path = api_path == "/" ? std::string("/*") : std::string(api_path) + "/*";
    return router.register_route(method, nested_path, std::move(nested_handler));
}

/// @brief Converts one route-pair registration failure into an API construction failure.
[[nodiscard]] FilesystemApiError
registration_failure(const HttpRouteRegistrationError error) noexcept
{
    return {FilesystemApiErrorCode::route_registration_failed, error};
}

} // namespace

/// @brief Registers filesystem endpoints for every validated runtime location.
Result<HttpRouter, FilesystemApiError>
make_filesystem_api_router(const configuration::runtime::ServerConfig &server)
{
    try
    {
        if (server.locations().empty())
        {
            return unexpected(FilesystemApiError{FilesystemApiErrorCode::missing_location, {}});
        }

        const auto mime_types = std::make_shared<const MimeTypeRegistry>(server.mime_types());
        HttpRouter router;
        for (const auto &location : server.locations())
        {
            auto read = register_location_routes(
                router, HttpMethod::get, location.api_path(),
                [location, mime_types](const HttpRequestView &, const HttpRouteParameters &)
                { return handle_resource(location, *mime_types, {}); },
                [location, mime_types](const HttpRequestView &,
                                       const HttpRouteParameters &parameters)
                { return handle_resource(location, *mime_types, parameters.wildcard_suffix()); });
            if (!read)
            {
                return unexpected(registration_failure(read.error()));
            }
            auto upload = register_location_routes(
                router, HttpMethod::put, location.api_path(),
                [location](const HttpRequestView &request, const HttpRouteParameters &)
                { return handle_upload(location, request, {}); },
                [location](const HttpRequestView &request, const HttpRouteParameters &parameters)
                { return handle_upload(location, request, parameters.wildcard_suffix()); });
            if (!upload)
            {
                return unexpected(registration_failure(upload.error()));
            }
            auto create = register_location_routes(
                router, HttpMethod::post, location.api_path(),
                [location](const HttpRequestView &request, const HttpRouteParameters &)
                { return handle_directory_creation(location, request, {}); },
                [location](const HttpRequestView &request, const HttpRouteParameters &parameters) {
                    return handle_directory_creation(location, request,
                                                     parameters.wildcard_suffix());
                });
            if (!create)
            {
                return unexpected(registration_failure(create.error()));
            }
            auto remove = register_location_routes(
                router, HttpMethod::delete_method, location.api_path(),
                [location](const HttpRequestView &request, const HttpRouteParameters &)
                { return handle_deletion(location, request, {}); },
                [location](const HttpRequestView &request, const HttpRouteParameters &parameters)
                { return handle_deletion(location, request, parameters.wildcard_suffix()); });
            if (!remove)
            {
                return unexpected(registration_failure(remove.error()));
            }
        }
        return router;
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
