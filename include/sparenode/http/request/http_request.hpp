#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sparenode/filesystem/temporary_file.hpp"

namespace sparenode::http
{

namespace detail
{
struct HttpRequestViewAccess;
}

/// @brief Identifies the HTTP methods accepted by the SpareNode v0.1 transport.
enum class HttpMethod : std::uint8_t
{
    get,           ///< Retrieve a resource.
    head,          ///< Retrieve response metadata without a body.
    post,          ///< Submit a bounded request body.
    put,           ///< Replace or upload a resource.
    delete_method, ///< Delete a resource.
    options        ///< Query endpoint capabilities.
};

/// @brief Borrows one parsed HTTP header from the caller-owned request buffer.
struct HttpHeaderView
{
    std::string_view name;  ///< Original case-preserving field name.
    std::string_view value; ///< Value with surrounding optional whitespace removed.
};

/// @brief Carries network identity after applying the configured proxy trust boundary.
struct HttpRequestTransportContext
{
    std::string peer_address;   ///< Direct numeric TCP peer address.
    std::string client_address; ///< Effective client address after trusted-proxy validation.
    bool secure{};              ///< Whether the client-facing request used protected transport.
    bool trusted_proxy{};       ///< Whether effective metadata came from the configured proxy.
};

/// @brief Borrows a complete parsed HTTP/1.1 request without copying its bytes.
///
/// Metadata views remain valid only while their original input buffer remains alive,
/// unmoved, and unmodified. Direct parsing may retain decoded body memory; a session
/// request instead shares a completed temporary-file artifact.
class HttpRequestView
{
  public:
    /// @brief Returns the validated supported HTTP method.
    /// @return Parsed request method.
    [[nodiscard]] HttpMethod method() const noexcept;

    /// @brief Returns the normalized routing target.
    /// @return Origin-form path/query or `*` for server-wide OPTIONS.
    [[nodiscard]] std::string_view target() const noexcept;

    /// @brief Returns all parsed header fields in their original source order.
    /// @return Read-only view over the bounded header collection.
    [[nodiscard]] std::span<const HttpHeaderView> fields() const noexcept;

    /// @brief Returns an in-memory decoded payload when direct parsing supplied one.
    /// @return Borrowed body, or an empty span for file-backed session ingestion.
    [[nodiscard]] std::span<const std::byte> body() const noexcept;

    /// @brief Returns the complete decoded payload size for memory or file-backed bodies.
    /// @return Exact decoded byte count after framing removal.
    [[nodiscard]] std::uint64_t body_size() const noexcept;

    /// @brief Shares the completed temporary body artifact when session ingestion used disk.
    /// @return Artifact owner, or null for memory-backed and zero-length bodies.
    [[nodiscard]] std::shared_ptr<filesystem::TemporaryFile> temporary_body() const noexcept;

    /// @brief Returns the cancellation and deadline policy retained for body publication.
    /// @return File-operation options active when session body ingestion completed.
    [[nodiscard]] filesystem::TemporaryFileIoOptions temporary_file_options() const noexcept;

    /// @brief Finds the first header using an ASCII case-insensitive name comparison.
    /// @param[in] name Header field name to locate.
    /// @return Borrowed header value, or an empty view when the field is absent.
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

    /// @brief Finds every header using an ASCII case-insensitive name comparison.
    /// @param[in] name Header field name to locate.
    /// @return Borrowed values in their original source order.
    [[nodiscard]] std::vector<std::string_view> headers(std::string_view name) const;

    /// @brief Returns validated direct and effective transport metadata.
    /// @return Context attached by the connection handler, or empty defaults for direct parsing.
    [[nodiscard]] const HttpRequestTransportContext &transport() const noexcept;

  private:
    friend struct detail::HttpRequestViewAccess;

    /// @brief Creates a complete view after every parser invariant has passed.
    /// @param[in] method Validated supported method.
    /// @param[in] target Validated normalized routing target.
    /// @param[in] headers Bounded validated header fields in source order.
    /// @param[in] body Exact borrowed body boundary.
    /// @param[in] body_size Exact decoded size for memory-backed or file-backed payloads.
    HttpRequestView(HttpMethod method, std::string_view target, std::vector<HttpHeaderView> headers,
                    std::span<const std::byte> body, std::uint64_t body_size);

    std::shared_ptr<const std::string> target_storage_; ///< Optional synthetic target owner.
    HttpMethod method_{};                               ///< Parsed supported request method.
    std::string_view target_;             ///< Normalized path/query or server-wide asterisk.
    std::vector<HttpHeaderView> headers_; ///< Bounded headers in source order.
    std::span<const std::byte> body_;     ///< Exact request body boundary.
    std::shared_ptr<const std::vector<std::byte>> body_storage_;  ///< Optional decoded-body owner.
    std::shared_ptr<filesystem::TemporaryFile> temporary_body_;   ///< Optional file-backed payload.
    filesystem::TemporaryFileIoOptions temporary_file_options_{}; ///< Publication I/O policy.
    std::uint64_t body_size_{};             ///< Decoded memory or temporary-file payload bytes.
    HttpRequestTransportContext transport_; ///< Validated peer, scheme, and proxy context.
};

/// @brief Returns the canonical uppercase spelling of a supported HTTP method.
/// @param[in] method Supported method to describe.
/// @return Static method text suitable for protocol diagnostics.
[[nodiscard]] std::string_view to_string(HttpMethod method) noexcept;

} // namespace sparenode::http
