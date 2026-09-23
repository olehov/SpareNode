#include "sparenode/http/filesystem_api.hpp"

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <new>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/configuration/runtime/share_config.hpp"
#include "sparenode/filesystem/directory_listing.hpp"
#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/http/http_request.hpp"
#include "sparenode/http/http_response.hpp"
#include "sparenode/http/http_status_code.hpp"

namespace sparenode::http
{
namespace
{

using configuration::runtime::ShareConfig;
using filesystem::DirectoryListingEntry;
using filesystem::DirectoryListingError;
using filesystem::DirectoryListingErrorCode;

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
[[nodiscard]] std::string serialize_listing(const ShareConfig &share,
                                            const std::vector<DirectoryListingEntry> &entries)
{
    std::string output;
    // Estimate 64 bytes for the outer object and share name, plus 96 per typical entry.
    // This only reduces reallocations; the string still grows when names require more space.
    output.reserve(64 + entries.size() * 96);
    output.append("{\"share\":");
    append_json_string(output, share.name());
    output.append(",\"entries\":[");
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
    case HttpStatusCode::bad_request:
        return "Bad Request";
    case HttpStatusCode::forbidden:
        return "Forbidden";
    case HttpStatusCode::not_found:
        return "Not Found";
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

/// @brief Handles one exact or wildcard directory-listing route.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
handle_listing(const ShareConfig &share, const std::string_view requested_path)
{
    try
    {
        if (!share.permissions().allows_read())
        {
            return make_json_response(HttpStatusCode::forbidden, "{\"error\":\"read_forbidden\"}");
        }
        const auto listing = filesystem::list_directory(share.root(), requested_path);
        if (!listing)
        {
            const auto [status, identifier] = public_error(listing.error());
            std::string body("{\"error\":");
            append_json_string(body, identifier);
            body.push_back('}');
            return make_json_response(status, body);
        }
        return make_json_response(HttpStatusCode::ok, serialize_listing(share, listing.value()));
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(HttpRouteError{
            HttpRouteErrorCode::handler_failure,
            static_cast<int>(DirectoryListingErrorCode::resource_allocation_failed)});
    }
}

} // namespace

Result<HttpRouter, FilesystemApiError>
make_filesystem_api_router(const configuration::runtime::ServerConfig &server)
{
    try
    {
        if (server.shares().empty())
        {
            return unexpected(FilesystemApiError{FilesystemApiErrorCode::missing_share, {}});
        }

        const auto share = server.shares().front();
        HttpRouter router;
        auto exact =
            router.register_route(HttpMethod::get, "/api/files",
                                  [share](const HttpRequestView &, const HttpRouteParameters &)
                                  { return handle_listing(share, {}); });
        if (!exact)
        {
            return unexpected(FilesystemApiError{FilesystemApiErrorCode::route_registration_failed,
                                                 exact.error()});
        }
        auto nested = router.register_route(
            HttpMethod::get, "/api/files/*",
            [share](const HttpRequestView &, const HttpRouteParameters &parameters)
            { return handle_listing(share, parameters.wildcard_suffix()); });
        if (!nested)
        {
            return unexpected(FilesystemApiError{FilesystemApiErrorCode::route_registration_failed,
                                                 nested.error()});
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
    case FilesystemApiErrorCode::missing_share:
        return "filesystem API requires a configured share";
    case FilesystemApiErrorCode::route_registration_failed:
        return "filesystem API route registration failed";
    case FilesystemApiErrorCode::resource_allocation_failed:
        return "filesystem API memory allocation failed";
    }
    return "unknown filesystem API error";
}

} // namespace sparenode::http
