#include "sparenode/http/http_response.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace sparenode::http
{
namespace
{

constexpr std::uint16_t minimum_response_status_code = 100;
constexpr std::uint16_t maximum_response_status_code = 599;

/// @brief Checks whether one byte is permitted in an HTTP field name.
[[nodiscard]] constexpr bool is_token_character(const char byte) noexcept
{
    if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z'))
    {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.find(byte) != std::string_view::npos;
}

/// @brief Rejects control bytes that could alter HTTP field framing.
[[nodiscard]] bool valid_field_value(const std::string_view value) noexcept
{
    return std::ranges::none_of(value, [](const unsigned char byte)
                                { return (byte < 0x20U && byte != '\t') || byte == 0x7FU; });
}

/// @brief Applies the HTTP reason-phrase byte policy.
[[nodiscard]] bool valid_reason_phrase(const std::string_view value) noexcept
{
    return valid_field_value(value);
}

/// @brief Folds one uppercase ASCII letter without locale-dependent behavior.
[[nodiscard]] constexpr char ascii_lower(const char byte) noexcept
{
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte;
}

/// @brief Compares framing-field names using ASCII case-insensitive semantics.
[[nodiscard]] constexpr bool ascii_case_insensitive_equal(const std::string_view left,
                                                          const std::string_view right) noexcept
{
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const char lhs, const char rhs)
                              { return ascii_lower(lhs) == ascii_lower(rhs); });
}

/// @brief Counts decimal digits without allocating temporary text.
[[nodiscard]] std::size_t decimal_length(const std::uint64_t value) noexcept
{
    std::array<char, 20> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return static_cast<std::size_t>(result.ptr - buffer.data());
}

/// @brief Identifies statuses for which this transport permits no message body.
[[nodiscard]] constexpr bool status_has_no_message_body(const HttpStatusCode status_code) noexcept
{
    const std::uint16_t value = http_status_code_value(status_code);
    return (value >= minimum_response_status_code &&
            value < http_status_code_value(HttpStatusCode::ok)) ||
           status_code == HttpStatusCode::no_content ||
           status_code == HttpStatusCode::reset_content ||
           status_code == HttpStatusCode::not_modified;
}

/// @brief Groups source position and capacity for one response-field check.
struct HeaderValidationContext
{
    std::size_t index{};
    std::size_t remaining{};
};

/// @brief Validates one response field and returns its serialized byte count.
[[nodiscard]] Result<std::size_t, HttpResponseValidationError>
validate_header(const HttpResponseHeader &header, const HeaderValidationContext context)
{
    if (header.name.empty() || !std::ranges::all_of(header.name, is_token_character))
    {
        return unexpected(HttpResponseValidationError{
            HttpResponseValidationErrorCode::invalid_header_name, context.index});
    }
    if (!valid_field_value(header.value))
    {
        return unexpected(HttpResponseValidationError{
            HttpResponseValidationErrorCode::invalid_header_value, context.index});
    }
    if (ascii_case_insensitive_equal(header.name, "Content-Length") ||
        ascii_case_insensitive_equal(header.name, "Transfer-Encoding"))
    {
        return unexpected(HttpResponseValidationError{
            HttpResponseValidationErrorCode::managed_framing_header, context.index});
    }
    if (header.name.size() > context.remaining ||
        header.value.size() > context.remaining - header.name.size() ||
        context.remaining - header.name.size() - header.value.size() < 4)
    {
        return unexpected(HttpResponseValidationError{
            HttpResponseValidationErrorCode::response_head_too_large, context.index});
    }
    return header.name.size() + header.value.size() + 4;
}

/// @brief Validates response metadata and predicts its serialized head boundary.
[[nodiscard]] Result<void, HttpResponseValidationError>
validate_response(const HttpStatusCode status_code, const std::string_view reason_phrase,
                  const std::span<const HttpResponseHeader> headers,
                  const std::uint64_t content_length)
{
    const std::uint16_t numeric_status = http_status_code_value(status_code);
    if (numeric_status < minimum_response_status_code ||
        numeric_status > maximum_response_status_code)
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::invalid_status_code, 0});
    }
    if (!valid_reason_phrase(reason_phrase))
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::invalid_reason_phrase, 0});
    }
    if (status_has_no_message_body(status_code) && content_length != 0)
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::body_not_allowed, 0});
    }
    if (headers.size() > HttpResponse::maximum_header_count)
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::too_many_headers, 0});
    }

    constexpr std::size_t status_line_fixed_bytes = 15;    // "HTTP/1.1 " + code + spaces + CRLF.
    constexpr std::size_t content_length_fixed_bytes = 18; // "Content-Length: " + CRLF.
    const std::size_t framing_bytes =
        status_has_no_message_body(status_code)
            ? 2
            : content_length_fixed_bytes + decimal_length(content_length) + 2;
    std::size_t head_size = status_line_fixed_bytes + reason_phrase.size() + framing_bytes;
    if (head_size > HttpResponse::maximum_head_bytes)
    {
        return unexpected(HttpResponseValidationError{
            HttpResponseValidationErrorCode::response_head_too_large, 0});
    }
    for (std::size_t index = 0; index < headers.size(); ++index)
    {
        const std::size_t remaining = HttpResponse::maximum_head_bytes - head_size;
        auto field_size = validate_header(headers[index], {index, remaining});
        if (!field_size)
        {
            return unexpected(field_size.error());
        }
        head_size += field_size.value();
    }
    return {};
}

} // namespace

/// @brief Stores one fully validated response representation.
/// @param[in] status_code Validated three-digit status.
/// @param[in] reason_phrase Validated reason text transferred into storage.
/// @param[in] headers Validated application fields transferred into storage.
/// @param[in] body Bounded memory body transferred into storage.
/// @param[in] content_length Exact framed body length.
/// @param[in] body_reader Optional incremental reader transferred into storage.
HttpResponse::HttpResponse(const HttpStatusCode status_code, std::string reason_phrase,
                           std::vector<HttpResponseHeader> headers, std::vector<std::byte> body,
                           const std::uint64_t content_length, HttpBodyReader body_reader)
    : status_code_(status_code), reason_phrase_(std::move(reason_phrase)),
      headers_(std::move(headers)), memory_body_(std::move(body)), content_length_(content_length),
      body_reader_(std::move(body_reader))
{
}

/// @brief Validates and owns one bounded in-memory response.
Result<HttpResponse, HttpResponseValidationError>
HttpResponse::create(const HttpStatusCode status_code, std::string reason_phrase,
                     std::vector<HttpResponseHeader> headers, std::vector<std::byte> body)
{
    if (body.size() > maximum_memory_body_bytes)
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::memory_body_too_large, 0});
    }
    if (auto validation = validate_response(status_code, reason_phrase, headers, body.size());
        !validation)
    {
        return unexpected(validation.error());
    }
    const std::uint64_t content_length = body.size();
    return HttpResponse(status_code, std::move(reason_phrase), std::move(headers), std::move(body),
                        content_length, {});
}

/// @brief Validates and owns one incremental response body source.
Result<HttpResponse, HttpResponseValidationError>
HttpResponse::create_streaming(const HttpStatusCode status_code, std::string reason_phrase,
                               std::vector<HttpResponseHeader> headers,
                               const std::uint64_t content_length, HttpBodyReader body_reader)
{
    if (!body_reader)
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::missing_body_reader, 0});
    }
    if (status_has_no_message_body(status_code))
    {
        return unexpected(
            HttpResponseValidationError{HttpResponseValidationErrorCode::body_not_allowed, 0});
    }
    if (auto validation = validate_response(status_code, reason_phrase, headers, content_length);
        !validation)
    {
        return unexpected(validation.error());
    }
    return HttpResponse(status_code, std::move(reason_phrase), std::move(headers), {},
                        content_length, std::move(body_reader));
}

/// @brief Returns the configured three-digit status code.
/// @return Status from 100 through 599.
HttpStatusCode HttpResponse::status_code() const noexcept
{
    return status_code_;
}

/// @brief Returns the immutable reason phrase.
/// @return Validated text without line delimiters.
std::string_view HttpResponse::reason_phrase() const noexcept
{
    return reason_phrase_;
}

/// @brief Returns immutable application-provided response fields.
/// @return Owned fields in their configured order.
std::span<const HttpResponseHeader> HttpResponse::headers() const noexcept
{
    return headers_;
}

/// @brief Returns the exact declared response body size.
/// @return Framed body byte count.
std::uint64_t HttpResponse::content_length() const noexcept
{
    return content_length_;
}

/// @brief Reports whether transmission must consume the body reader.
/// @return `true` for an incremental one-shot response.
bool HttpResponse::is_streaming() const noexcept
{
    return static_cast<bool>(body_reader_);
}

/// @brief Returns owned in-memory bytes or an empty span for a stream.
/// @return Immutable body storage.
std::span<const std::byte> HttpResponse::memory_body() const noexcept
{
    return memory_body_;
}

/// @brief Converts a response validation category to stable diagnostic text.
const char *to_string(const HttpResponseValidationErrorCode code) noexcept
{
    switch (code)
    {
    case HttpResponseValidationErrorCode::invalid_status_code:
        return "HTTP response status code is invalid";
    case HttpResponseValidationErrorCode::invalid_reason_phrase:
        return "HTTP response reason phrase is invalid";
    case HttpResponseValidationErrorCode::too_many_headers:
        return "HTTP response contains too many headers";
    case HttpResponseValidationErrorCode::invalid_header_name:
        return "HTTP response header name is invalid";
    case HttpResponseValidationErrorCode::invalid_header_value:
        return "HTTP response header value is invalid";
    case HttpResponseValidationErrorCode::managed_framing_header:
        return "HTTP response framing header is managed by the transport";
    case HttpResponseValidationErrorCode::body_not_allowed:
        return "HTTP response status does not permit a message body";
    case HttpResponseValidationErrorCode::response_head_too_large:
        return "HTTP response head exceeds its configured boundary";
    case HttpResponseValidationErrorCode::memory_body_too_large:
        return "HTTP response in-memory body exceeds its configured boundary";
    case HttpResponseValidationErrorCode::missing_body_reader:
        return "HTTP streaming response requires a body reader";
    }
    return "unknown HTTP response validation error";
}

} // namespace sparenode::http
