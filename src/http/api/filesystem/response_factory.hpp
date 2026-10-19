#pragma once

#include <string>

#include "sparenode/foundation/result.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/routing/http_router.hpp"

namespace sparenode::http::detail
{

/// @brief Creates a validated JSON response for a route handler.
[[nodiscard]] Result<HttpResponse, HttpRouteError> make_json_response(HttpStatusCode status,
                                                                      const std::string &body);

} // namespace sparenode::http::detail
