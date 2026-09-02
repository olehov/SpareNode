#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "sparenode/http/http_status_code.hpp"
#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Owns one application-provided HTTP response field.
struct HttpResponseHeader
{
    std::string name;  ///< Case-preserving HTTP field name.
    std::string value; ///< Field value without line delimiters.
};

/// @brief Identifies which body-producing subsystem reported a stream failure.
enum class HttpBodyReadErrorDomain : std::uint8_t
{
    application, ///< An application-owned body producer failed.
    filesystem,  ///< A filesystem-backed body producer failed.
    cancellation ///< The body producer observed cancellation.
};

/// @brief Preserves a portable body-source domain and producer-defined code.
struct HttpBodyReadError
{
    HttpBodyReadErrorDomain domain{}; ///< Subsystem that could not produce more bytes.
    int code{};                       ///< Producer-defined portable or native code.
};

/// @brief Produces at most one caller-bounded chunk of a streaming response body.
///
/// A zero-byte success means end of input. A positive success must not exceed the
/// supplied destination size. The callable owns any cursor state it requires.
using HttpBodyReader = std::move_only_function<Result<std::size_t, HttpBodyReadError>(
    std::span<std::byte> destination, const std::stop_token &stop_token)>;

/// @brief Identifies an invalid response rejected before transmission begins.
enum class HttpResponseValidationErrorCode : std::uint8_t
{
    invalid_status_code,     ///< Status is outside the three-digit HTTP range.
    invalid_reason_phrase,   ///< Reason contains a forbidden control byte.
    too_many_headers,        ///< Header count exceeds the response boundary.
    invalid_header_name,     ///< A field name is empty or is not an HTTP token.
    invalid_header_value,    ///< A field value contains a forbidden control byte.
    managed_framing_header,  ///< Content-Length or Transfer-Encoding was supplied.
    body_not_allowed,        ///< This status code cannot carry a message body.
    response_head_too_large, ///< Serialized status and headers exceed their boundary.
    memory_body_too_large,   ///< An in-memory body exceeds its dedicated boundary.
    missing_body_reader      ///< A streaming response has no callable body source.
};

/// @brief Describes one response-construction failure and its optional field index.
struct HttpResponseValidationError
{
    HttpResponseValidationErrorCode code{}; ///< Stable validation failure category.
    std::size_t header_index{};             ///< Related header index, otherwise zero.
};

/// @brief Owns one validated HTTP/1.1 response and its bounded body source.
class HttpResponse final
{
  public:
    /// @brief Maximum number of application-provided response fields.
    static constexpr std::size_t maximum_header_count = 100;
    /// @brief Maximum serialized status-line and header bytes.
    static constexpr std::size_t maximum_head_bytes = 32768;
    /// @brief Maximum body bytes retained directly by one response object.
    static constexpr std::size_t maximum_memory_body_bytes = std::size_t{1024} * 1024;

    /// @brief Creates a validated response that owns a small in-memory body.
    /// @param[in] status_code Three-digit HTTP status from 100 through 599.
    /// @param[in] reason_phrase Human-readable reason without line delimiters.
    /// @param[in] headers Application fields excluding transport-managed framing.
    /// @param[in] body Body bytes copied or moved into bounded response storage.
    /// @return Complete response or a structured validation failure.
    [[nodiscard]] static Result<HttpResponse, HttpResponseValidationError>
    create(HttpStatusCode status_code, std::string reason_phrase,
           std::vector<HttpResponseHeader> headers, std::vector<std::byte> body);

    /// @brief Creates a validated response backed by an incremental body producer.
    /// @param[in] status_code Three-digit HTTP status from 100 through 599.
    /// @param[in] reason_phrase Human-readable reason without line delimiters.
    /// @param[in] headers Application fields excluding transport-managed framing.
    /// @param[in] content_length Exact number of bytes the reader must produce.
    /// @param[in] body_reader Stateful reader transferred into the response.
    /// @return Complete streaming response or a structured validation failure.
    [[nodiscard]] static Result<HttpResponse, HttpResponseValidationError>
    create_streaming(HttpStatusCode status_code, std::string reason_phrase,
                     std::vector<HttpResponseHeader> headers, std::uint64_t content_length,
                     HttpBodyReader body_reader);

    /// @brief Transfers complete response and body-reader ownership.
    /// @param[in,out] other Response whose owned state is transferred.
    HttpResponse(HttpResponse &&other) noexcept = default;
    /// @brief Replaces this response with transferred response state.
    /// @param[in,out] other Response whose owned state is transferred.
    /// @return This response after replacement.
    HttpResponse &operator=(HttpResponse &&other) noexcept = default;
    HttpResponse(const HttpResponse &) = delete;
    HttpResponse &operator=(const HttpResponse &) = delete;
    /// @brief Releases owned headers, memory body, and streaming-reader state.
    ~HttpResponse() = default;

    /// @brief Returns the validated HTTP status code.
    /// @return Three-digit status from 100 through 599.
    [[nodiscard]] HttpStatusCode status_code() const noexcept;
    /// @brief Returns the validated reason phrase.
    /// @return Immutable reason text without protocol delimiters.
    [[nodiscard]] std::string_view reason_phrase() const noexcept;
    /// @brief Returns application fields in their configured order.
    /// @return Immutable owned header collection.
    [[nodiscard]] std::span<const HttpResponseHeader> headers() const noexcept;
    /// @brief Returns the exact Content-Length generated by the transport.
    /// @return Declared body byte count.
    [[nodiscard]] std::uint64_t content_length() const noexcept;
    /// @brief Reports whether the body is produced incrementally.
    /// @return `true` when transmission consumes the owned reader.
    [[nodiscard]] bool is_streaming() const noexcept;
    /// @brief Returns the complete body for a non-streaming response.
    /// @return Immutable owned bytes, or an empty span for streaming responses.
    [[nodiscard]] std::span<const std::byte> memory_body() const noexcept;

  private:
    friend class HttpResponseWriterAccess;

    /// @brief Stores state after every public response invariant has passed.
    /// @param[in] status_code Validated three-digit status.
    /// @param[in] reason_phrase Validated reason text transferred into storage.
    /// @param[in] headers Validated application fields transferred into storage.
    /// @param[in] body Bounded memory body transferred into storage.
    /// @param[in] content_length Exact framed body length.
    /// @param[in] body_reader Optional incremental reader transferred into storage.
    HttpResponse(HttpStatusCode status_code, std::string reason_phrase,
                 std::vector<HttpResponseHeader> headers, std::vector<std::byte> body,
                 std::uint64_t content_length, HttpBodyReader body_reader);

    HttpStatusCode status_code_{};            ///< Validated typed response status.
    std::string reason_phrase_;               ///< Validated reason text.
    std::vector<HttpResponseHeader> headers_; ///< Validated application fields.
    std::vector<std::byte> memory_body_;      ///< Bounded non-streaming body.
    std::uint64_t content_length_{};          ///< Exact framed body byte count.
    HttpBodyReader body_reader_;              ///< Stateful one-shot streaming source.
};

/// @brief Returns stable text for one response validation failure.
[[nodiscard]] const char *to_string(HttpResponseValidationErrorCode code) noexcept;

} // namespace sparenode::http
