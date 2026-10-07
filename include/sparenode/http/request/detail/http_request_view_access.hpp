#pragma once

#include "sparenode/http/request/http_request_parser.hpp"

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
        HttpRequestView request(head.method, head.target, std::move(head.fields), body,
                                body.size());
        request.body_storage_ = std::move(storage);
        request.target_storage_ = std::move(head.target_storage);
        return request;
    }

    /// @brief Combines validated metadata with a completed temporary body artifact.
    /// @param[in] head Validated metadata whose source remains alive.
    /// @param[in] body_size Exact decoded bytes persisted in the artifact.
    /// @param[in] artifact Optional completed artifact; null represents an empty body.
    /// @param[in] options Cancellation and deadline retained for later publication.
    /// @return Request retaining metadata views and shared temporary-file ownership.
    [[nodiscard]] static HttpRequestView
    create_file_backed(HttpRequestHead head, const std::uint64_t body_size,
                       std::shared_ptr<filesystem::TemporaryFile> artifact,
                       const filesystem::TemporaryFileIoOptions &options = {})
    {
        HttpRequestView request(head.method, head.target, std::move(head.fields), {}, body_size);
        request.temporary_body_ = std::move(artifact);
        request.temporary_file_options_ = options;
        request.target_storage_ = std::move(head.target_storage);
        return request;
    }
};
} // namespace sparenode::http::detail
