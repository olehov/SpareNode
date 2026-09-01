#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

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
    std::size_t max_body_bytes{std::size_t{1024} * 1024}; ///< Maximum accepted Content-Length.
};

/// @brief Identifies why an HTTP request cannot be parsed safely.
enum class HttpRequestParseErrorCode : std::uint8_t
{
    request_line_too_large,       ///< Request line exceeds its configured byte limit.
    headers_too_large,            ///< Header section exceeds its configured byte limit.
    too_many_headers,             ///< Header count exceeds its configured limit.
    body_too_large,               ///< Content-Length exceeds its configured limit.
    invalid_line_ending,          ///< A protocol line does not end in CRLF.
    malformed_request_line,       ///< Request line does not contain exactly three components.
    invalid_method,               ///< Method is not a valid HTTP token.
    unsupported_method,           ///< Valid method is outside the SpareNode v0.1 subset.
    invalid_request_target,       ///< Target is not a safe origin-form target.
    unsupported_http_version,     ///< Version is not HTTP/1.1.
    malformed_header,             ///< Header name or value violates HTTP field syntax.
    folded_header,                ///< Obsolete line folding was supplied.
    missing_host,                 ///< Required HTTP/1.1 Host field is absent or empty.
    duplicate_host,               ///< More than one Host field was supplied.
    invalid_content_length,       ///< Content-Length is empty, non-decimal, or overflowing.
    duplicate_content_length,     ///< More than one Content-Length field was supplied.
    unsupported_transfer_encoding ///< Transfer-Encoding is intentionally unsupported.
};

/// @brief Preserves the portable HTTP parse failure and its input byte offset.
struct HttpRequestParseError
{
    HttpRequestParseErrorCode code{}; ///< Stable failure category.
    std::size_t byte_offset{};        ///< Zero-based position associated with the failure.
};

/// @brief Represents either an incomplete buffer or one complete borrowed request.
struct HttpRequestParseResult
{
    bool complete{};              ///< Whether `request` contains a complete request.
    std::size_t consumed_bytes{}; ///< Exact bytes belonging to the completed request.
    HttpRequestView request;      ///< Valid only when `complete` is true.
};

/// @brief Parses one bounded HTTP/1.1 request from a caller-owned byte buffer.
///
/// Incomplete input is not an error and returns `complete == false`. The caller
/// may append more bytes and invoke the function again. On completion,
/// `consumed_bytes` excludes any bytes belonging to a following pipelined request.
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
