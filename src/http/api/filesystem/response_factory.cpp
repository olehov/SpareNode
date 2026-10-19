#include "response_factory.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/http/response/http_response.hpp"

namespace sparenode::http::detail
{
namespace
{

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

} // namespace

Result<HttpResponse, HttpRouteError> make_json_response(const HttpStatusCode status,
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

} // namespace sparenode::http::detail
