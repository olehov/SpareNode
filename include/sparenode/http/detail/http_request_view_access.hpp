#pragma once

#include "sparenode/http/http_request_parser.hpp"

namespace sparenode::http::detail
{
/// @brief Constructs immutable views after validated head and body ingestion.
struct HttpRequestViewAccess
{
    /// @brief Combines validated metadata with complete decoded body storage.
    /// @param[in] head Validated metadata whose source remains alive.
    /// @param[in] body Complete payload borrowed for the request lifetime.
    /// @param[in] storage Optional immutable owner used for decoded chunked payloads.
    /// @return Request retaining metadata views and optional body ownership.
    [[nodiscard]] static HttpRequestView
    create(HttpRequestHead head, std::span<const std::byte> body,
           std::shared_ptr<const std::vector<std::byte>> storage = {})
    {
        HttpRequestView request(head.method, head.target, std::move(head.fields), body);
        request.body_storage_ = std::move(storage);
        request.target_storage_ = std::move(head.target_storage);
        return request;
    }
};
} // namespace sparenode::http::detail
