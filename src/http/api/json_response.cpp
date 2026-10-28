#include "sparenode/http/api/json_response.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sparenode::http::api
{

Result<HttpResponse, HttpRouteError>
make_json_response(const HttpStatusCode status, const std::string_view body,
                   std::vector<HttpResponseHeader> additional_headers)
{
    const auto bytes = std::as_bytes(std::span(body.data(), body.size()));
    std::vector<std::byte> owned_body(bytes.begin(), bytes.end());
    additional_headers.insert(
        additional_headers.begin(),
        HttpResponseHeader{"Content-Type", "application/json; charset=utf-8"});
    auto response = HttpResponse::create(status, std::string(http_reason_phrase(status)),
                                         std::move(additional_headers), std::move(owned_body));
    if (!response)
    {
        return unexpected(HttpRouteError{HttpRouteErrorCode::response_validation_failure,
                                         static_cast<int>(response.error().code)});
    }
    return std::move(response).value();
}

} // namespace sparenode::http::api
