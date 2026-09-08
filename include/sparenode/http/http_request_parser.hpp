#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "sparenode/http/http_request.hpp"
#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Defines explicit memory and protocol boundaries for one HTTP request.
struct HttpRequestParserLimits
{
    std::size_t max_request_line_bytes{8192}; ///< Request line excluding its CRLF.
    std::size_t max_header_bytes{32768};      ///< Header section including terminating CRLF.
    std::size_t max_header_count{100};        ///< Maximum parsed header fields.
    std::size_t max_body_bytes{std::size_t{1024} * 1024}; ///< Maximum decoded request payload.
    std::size_t max_chunk_line_bytes{1024};      ///< Chunk size and extensions excluding CRLF.
    std::size_t max_chunk_metadata_bytes{65536}; ///< Cumulative non-payload chunk framing bytes.
    std::size_t max_trailer_bytes{8192};         ///< Trailer section including final CRLF.
    std::size_t max_trailer_count{32};           ///< Maximum discarded trailer fields.
};

/// @brief Identifies why an HTTP request cannot be parsed safely.
enum class HttpRequestParseErrorCode : std::uint8_t
{
    request_line_too_large,        ///< Request line exceeds its configured byte limit.
    headers_too_large,             ///< Header section exceeds its configured byte limit.
    too_many_headers,              ///< Header count exceeds its configured limit.
    body_too_large,                ///< Content-Length exceeds its configured limit.
    invalid_line_ending,           ///< A protocol line does not end in CRLF.
    malformed_request_line,        ///< Request line does not contain exactly three components.
    invalid_method,                ///< Method is not a valid HTTP token.
    unsupported_method,            ///< Valid method is outside the SpareNode v0.1 subset.
    invalid_request_target,        ///< Target is outside the supported URI forms.
    unsupported_http_version,      ///< Well-formed version is outside HTTP/1.1 through 1.9.
    malformed_header,              ///< Header name or value violates HTTP field syntax.
    folded_header,                 ///< Obsolete line folding was supplied.
    missing_host,                  ///< Required HTTP/1.1 Host field is absent or empty.
    duplicate_host,                ///< More than one Host field was supplied.
    invalid_host,                  ///< Host does not match the supported authority grammar.
    invalid_content_length,        ///< Content-Length is empty, non-decimal, or overflowing.
    duplicate_content_length,      ///< More than one Content-Length field was supplied.
    unsupported_transfer_encoding, ///< A transfer coding other than chunked was supplied.
    invalid_transfer_encoding,     ///< Transfer-Encoding syntax or ordering is ambiguous.
    conflicting_message_length,    ///< Both Transfer-Encoding and Content-Length are present.
    malformed_chunk,               ///< Chunk framing or extension syntax is invalid or overflows.
    chunk_metadata_too_large,      ///< Chunk framing exceeds a line or cumulative bound.
    invalid_trailer,               ///< Trailer syntax or a forbidden trailer field was supplied.
    trailers_too_large,            ///< Trailer count or bytes exceed their configured limit.
    incomplete_body,               ///< EOF occurred before the declared body framing completed.
    malformed_http_version         ///< Version token violates HTTP/DIGIT.DIGIT syntax.
};

/// @brief Preserves the portable HTTP parse failure and its input byte offset.
struct HttpRequestParseError
{
    HttpRequestParseErrorCode code{}; ///< Stable failure category.
    std::size_t byte_offset{};        ///< Zero-based position associated with the failure.
};

/// @brief Validated request head borrowing the supplied input, before body ingestion.
struct HttpRequestHead
{
    HttpMethod method{};                  ///< Validated supported method.
    std::string_view target{};            ///< Borrowed validated target.
    std::vector<HttpHeaderView> fields{}; ///< Effective fields; absolute authority replaces Host.
    std::size_t consumed_bytes{};         ///< Wire offset immediately after the head.
    std::size_t content_length{};         ///< Validated fixed length, zero when absent.
    bool chunked{}; ///< Selects chunked decoding instead of fixed-length ingestion.
    std::shared_ptr<const std::string> target_storage{}; ///< Optional normalized target owner.
};

/// @brief Parses only request metadata so the body can be ingested incrementally.
/// @param[in] input Caller-owned request bytes retained for returned views.
/// @param[in] limits Header and body validation limits.
/// @return Validated head, no value when incomplete, or a structured error.
[[nodiscard]] Result<std::optional<HttpRequestHead>, HttpRequestParseError>
parse_http_request_head(std::span<const std::byte> input,
                        const HttpRequestParserLimits &limits = {});

/// @brief Represents immutable progress for one borrowed HTTP request.
class HttpRequestParseResult final
{
  public:
    /// @brief Creates the state returned while more input bytes are required.
    /// @return Incomplete progress with no request and zero consumed bytes.
    [[nodiscard]] static HttpRequestParseResult incomplete() noexcept
    {
        return HttpRequestParseResult();
    }

    /// @brief Creates progress containing one complete validated request.
    /// @param[in] consumed_bytes Exact bytes belonging to the request.
    /// @param[in] request Complete borrowed request view.
    /// @return Complete internally consistent parse progress.
    [[nodiscard]] static HttpRequestParseResult complete(std::size_t consumed_bytes,
                                                         HttpRequestView request)
    {
        return HttpRequestParseResult(consumed_bytes, std::move(request));
    }

    /// @brief Reports whether a complete request is available.
    /// @return `true` exactly when request() contains a value.
    [[nodiscard]] bool is_complete() const noexcept
    {
        return request_.has_value();
    }

    /// @brief Returns the complete request when enough bytes were supplied.
    /// @return Borrowed request wrapped in an optional, or no value while incomplete.
    [[nodiscard]] const std::optional<HttpRequestView> &request() const noexcept
    {
        return request_;
    }

    /// @brief Returns the exact completed request boundary.
    /// @return Request byte count, or zero while incomplete.
    [[nodiscard]] std::size_t consumed_bytes() const noexcept
    {
        return consumed_bytes_;
    }

  private:
    HttpRequestParseResult() = default;

    /// @brief Stores one complete parse state.
    /// @param[in] consumed_bytes Exact bytes belonging to the request.
    /// @param[in] request Complete borrowed request view.
    HttpRequestParseResult(const std::size_t consumed_bytes, HttpRequestView request)
        : consumed_bytes_(consumed_bytes), request_(std::move(request))
    {
    }

    std::size_t consumed_bytes_{};           ///< Zero or the complete request byte count.
    std::optional<HttpRequestView> request_; ///< Present exactly for complete input.
};

/// @brief Parses one bounded HTTP/1.1 request from a caller-owned byte buffer.
///
/// Incomplete input is not an error and returns `is_complete() == false`. The caller
/// may append more bytes and invoke the function again. On completion,
/// `consumed_bytes()` excludes bytes belonging to a following pipelined request.
/// Chunked bodies retain bounded decoded storage in the returned request. Metadata
/// still borrows input. For streaming ingestion, use parse_http_request_head() and
/// HttpBodyDecoder rather than repeatedly parsing a growing full request buffer.
/// @param[in] input Contiguous request bytes retained for every returned view.
/// @param[in] limits Explicit request-line, header, count, and body boundaries.
/// @return Parse progress or a structured terminal protocol failure.
[[nodiscard]] Result<HttpRequestParseResult, HttpRequestParseError>
parse_http_request(std::span<const std::byte> input, const HttpRequestParserLimits &limits = {});

/// @brief Returns a concise description of an HTTP request parse failure.
/// @param[in] code Portable parse failure category.
/// @return Static English text suitable for diagnostics and tests.
[[nodiscard]] const char *to_string(HttpRequestParseErrorCode code) noexcept;

} // namespace sparenode::http
