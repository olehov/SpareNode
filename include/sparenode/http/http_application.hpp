#pragma once

#include <cstdint>
#include <functional>

#include "sparenode/foundation/result.hpp"
#include "sparenode/http/request/http_request.hpp"
#include "sparenode/http/response/http_response.hpp"

namespace sparenode::http
{

/// @brief Identifies a transport-neutral HTTP application failure.
enum class HttpApplicationErrorCode : std::uint8_t
{
    request_failed ///< The application could not produce a valid response.
};

/// @brief Preserves a stable application failure category and optional subsystem detail.
struct HttpApplicationError
{
    HttpApplicationErrorCode code{}; ///< Stable application failure category.
    int detail{};                    ///< Application-defined diagnostic detail, otherwise zero.
};

/// @brief Produces one response for a complete validated HTTP request.
///
/// Implementations may be called concurrently by transport workers and must synchronize any
/// captured mutable state. The callback has no dependency on TCP connection ownership.
using HttpApplication =
    std::function<Result<HttpResponse, HttpApplicationError>(const HttpRequestView &request)>;

} // namespace sparenode::http
