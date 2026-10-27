#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "sparenode/http/request/http_request_parser.hpp"

namespace sparenode::http
{

/// @brief Selects where decoded request bytes remain until route dispatch.
enum class RequestBodyStorageMode : std::uint8_t
{
    temporary_file, ///< Persist payload outside request memory for filesystem operations.
    memory          ///< Retain a small sensitive payload only in process memory.
};

/// @brief Chooses request-body storage and a route-specific decoded-byte limit.
struct RequestBodyStoragePolicy
{
    RequestBodyStorageMode mode{RequestBodyStorageMode::temporary_file}; ///< Storage destination.
    std::size_t max_body_bytes{}; ///< Zero retains the connection-wide decoded-byte limit.
};

/// @brief Selects storage after validated request metadata is available.
using RequestBodyStoragePolicySelector =
    std::function<RequestBodyStoragePolicy(const HttpRequestHead &)>;

} // namespace sparenode::http
