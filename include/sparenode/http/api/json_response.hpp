#pragma once

#include <string_view>
#include <vector>

#include "sparenode/foundation/result.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/response/http_response.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http::api
{

/// @brief Creates a validated JSON response with the canonical HTTP reason phrase.
/// @param[in] status Standard response status.
/// @param[in] body UTF-8 JSON bytes copied into bounded response storage.
/// @param[in] additional_headers Optional API-specific fields excluding Content-Type.
/// @return Complete response or a stable route-level validation failure.
[[nodiscard]] Result<HttpResponse, HttpRouteError>
make_json_response(HttpStatusCode status, std::string_view body,
                   std::vector<HttpResponseHeader> additional_headers = {});

} // namespace sparenode::http::api
