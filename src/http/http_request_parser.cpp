#include "sparenode/http/http_request_parser.hpp"

#include "sparenode/http/detail/host_authority.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

namespace sparenode::http
{
namespace detail
{

/// @brief Constructs immutable request views exclusively for the validated parser path.
struct HttpRequestViewAccess
{
    /// @brief Forwards validated fields into the private complete-view constructor.
    /// @param[in] method Validated supported method.
    /// @param[in] target Validated origin-form target.
    /// @param[in] headers Bounded validated header fields.
    /// @param[in] body Exact borrowed request body.
    /// @return Complete immutable request view.
    [[nodiscard]] static HttpRequestView create(HttpMethod method, std::string_view target,
                                                std::vector<HttpHeaderView> headers,
                                                std::span<const std::byte> body)
    {
        return HttpRequestView(method, target, std::move(headers), body);
    }
};

} // namespace detail

namespace
{

/// @brief Number of bytes in one percent-encoded URI sequence (`%HH`).
constexpr std::size_t percent_encoded_sequence_size = 3;

/// @brief Stores the byte offset immediately before a validated CRLF delimiter.
struct LineEnd
{
    std::size_t offset{}; ///< Offset of the carriage return that begins the CRLF pair.
};

/// @brief Groups offsets and limits required while scanning a header line.
struct HeaderLineSearch
{
    std::size_t line_start{};          ///< Offset at which the current header line begins.
    std::size_t header_start{};        ///< Offset at which the header section begins.
    std::size_t maximum_header_size{}; ///< Maximum header bytes including the final CRLF.
};

/// @brief Collects the bounded header result needed to finish one request.
struct ParsedHeaderSection
{
    std::vector<HttpHeaderView> headers; ///< Parsed fields in source order.
    std::size_t body_start{};            ///< Offset immediately after the header terminator.
    std::size_t body_size{};             ///< Strict parsed Content-Length, or zero when absent.
};

/// @brief Preserves one validated field and the positions needed for diagnostics.
struct ParsedHeaderField
{
    HttpHeaderView field;      ///< Borrowed validated name and trimmed value.
    std::size_t line_start{};  ///< Offset at which the complete header line begins.
    std::size_t value_start{}; ///< Offset immediately after the field-name colon.
};

/// @brief Accumulates bounded header and framing state during one parse.
struct HeaderParseState
{
    std::vector<HttpHeaderView> headers;  ///< Parsed fields in source order.
    std::optional<std::size_t> body_size; ///< Validated Content-Length when supplied.
    bool host_seen{};                     ///< Whether the required Host field was observed.
};

/// @brief Searches for and validates the request-line terminator within its byte limit.
/// @param[in] input Complete caller-provided byte view.
/// @param[in] maximum_size Maximum request-line bytes excluding CRLF.
/// @return A terminator when available, no value for incomplete input, or a protocol error.
[[nodiscard]] Result<std::optional<LineEnd>, HttpRequestParseError>
find_request_line_end(const std::string_view input, const std::size_t maximum_size)
{
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        if (input[index] == '\n')
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::invalid_line_ending, index});
        }
        if (input[index] == '\r')
        {
            if (index + 1 >= input.size())
            {
                return std::optional<LineEnd>{};
            }
            if (input[index + 1] != '\n')
            {
                return unexpected(
                    HttpRequestParseError{HttpRequestParseErrorCode::invalid_line_ending, index});
            }
            if (index > maximum_size)
            {
                return unexpected(HttpRequestParseError{
                    HttpRequestParseErrorCode::request_line_too_large, maximum_size});
            }
            return std::optional<LineEnd>{LineEnd{index}};
        }
        if (index >= maximum_size)
        {
            return unexpected(HttpRequestParseError{
                HttpRequestParseErrorCode::request_line_too_large, maximum_size});
        }
    }
    return std::optional<LineEnd>{};
}

/// @brief Searches for one header-line terminator while enforcing the total header limit.
/// @param[in] input Complete caller-provided byte view.
/// @param[in] search Grouped source offsets and configured header boundary.
/// @return A terminator when available, no value for incomplete input, or a protocol error.
[[nodiscard]] Result<std::optional<LineEnd>, HttpRequestParseError>
find_header_line_end(const std::string_view input, const HeaderLineSearch &search)
{
    for (std::size_t index = search.line_start; index < input.size(); ++index)
    {
        if (index - search.header_start >= search.maximum_header_size)
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::headers_too_large,
                                      search.header_start + search.maximum_header_size});
        }
        if (input[index] == '\n')
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::invalid_line_ending, index});
        }
        if (input[index] != '\r')
        {
            continue;
        }
        if (index + 1 >= input.size())
        {
            return std::optional<LineEnd>{};
        }
        if (input[index + 1] != '\n')
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::invalid_line_ending, index});
        }
        if (index + 2 - search.header_start > search.maximum_header_size)
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::headers_too_large,
                                      search.header_start + search.maximum_header_size});
        }
        return std::optional<LineEnd>{LineEnd{index}};
    }
    return std::optional<LineEnd>{};
}

/// @brief Checks whether one ASCII byte is allowed in an HTTP token.
/// @param[in] byte Byte to classify without locale-dependent behavior.
/// @return `true` for an RFC token character.
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

/// @brief Compares ASCII protocol names without locale-dependent case conversion.
/// @param[in] left First byte sequence.
/// @param[in] right Second byte sequence.
/// @return `true` when both sequences differ only by ASCII letter case.
[[nodiscard]] constexpr bool ascii_case_insensitive_equal(const std::string_view left,
                                                          const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const char left_byte = left[index];
        const char right_byte = right[index];
        const char normalized_left = left_byte >= 'A' && left_byte <= 'Z'
                                         ? static_cast<char>(left_byte + ('a' - 'A'))
                                         : left_byte;
        const char normalized_right = right_byte >= 'A' && right_byte <= 'Z'
                                          ? static_cast<char>(right_byte + ('a' - 'A'))
                                          : right_byte;
        if (normalized_left != normalized_right)
        {
            return false;
        }
    }
    return true;
}

/// @brief Validates and maps the request method token into the supported method set.
/// @param[in] method Method bytes from the request line.
/// @param[in] offset Source offset associated with a method failure.
/// @return Parsed method or a structured invalid/unsupported method error.
[[nodiscard]] Result<HttpMethod, HttpRequestParseError> parse_method(const std::string_view method,
                                                                     const std::size_t offset)
{
    if (method.empty())
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::malformed_request_line, offset});
    }
    if (!std::ranges::all_of(method, is_token_character))
    {
        return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::invalid_method, offset});
    }
    if (method == "GET")
    {
        return HttpMethod::get;
    }
    if (method == "HEAD")
    {
        return HttpMethod::head;
    }
    if (method == "POST")
    {
        return HttpMethod::post;
    }
    if (method == "PUT")
    {
        return HttpMethod::put;
    }
    if (method == "DELETE")
    {
        return HttpMethod::delete_method;
    }
    if (method == "OPTIONS")
    {
        return HttpMethod::options;
    }
    return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::unsupported_method, offset});
}

/// @brief Checks whether one byte is an ASCII hexadecimal digit.
/// @param[in] byte Byte to classify without locale-dependent behavior.
/// @return `true` for `0-9`, `A-F`, or `a-f`.
[[nodiscard]] constexpr bool is_hexadecimal_digit(const unsigned char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') ||
           (byte >= 'a' && byte <= 'f');
}

/// @brief Checks whether one byte belongs to the RFC 3986 `pchar` set.
/// @param[in] byte Unescaped request-target byte to classify.
/// @return `true` for an unreserved, sub-delimiter, colon, or at-sign byte.
[[nodiscard]] constexpr bool is_path_character(const unsigned char byte) noexcept
{
    const bool is_alpha_numeric = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                                  (byte >= 'a' && byte <= 'z');
    constexpr std::string_view punctuation = "-._~!$&'()*+,;=:@";
    return is_alpha_numeric || punctuation.find(static_cast<char>(byte)) != std::string_view::npos;
}

/// @brief Validates an RFC 3986 origin-form path and optional query.
/// @param[in] target Request-target bytes from the request line.
/// @return `true` when every path, query, and percent-encoded byte is grammatical.
[[nodiscard]] bool valid_request_target(const std::string_view target) noexcept
{
    if (target.empty() || target.front() != '/')
    {
        return false;
    }

    bool query_started = false;
    for (std::size_t index = 0; index < target.size(); ++index)
    {
        const auto byte = static_cast<unsigned char>(target[index]);
        if (byte == '%')
        {
            if (target.size() - index < percent_encoded_sequence_size)
            {
                return false;
            }
            const std::string_view encoded_digits =
                target.substr(index + 1, percent_encoded_sequence_size - 1);
            if (!std::ranges::all_of(
                    encoded_digits, [](const char digit)
                    { return is_hexadecimal_digit(static_cast<unsigned char>(digit)); }))
            {
                return false;
            }
            index += percent_encoded_sequence_size - 1;
            continue;
        }
        if (!query_started && byte == '?')
        {
            query_started = true;
            continue;
        }
        if (byte == '/' || (query_started && byte == '?'))
        {
            continue;
        }
        if (!is_path_character(byte))
        {
            return false;
        }
    }
    return true;
}

/// @brief Parses a strict unsigned decimal Content-Length value.
/// @param[in] value Trimmed header value to convert.
/// @param[in] offset Source offset associated with a conversion failure.
/// @return Parsed length or an invalid-content-length error.
[[nodiscard]] Result<std::size_t, HttpRequestParseError>
parse_content_length(const std::string_view value, const std::size_t offset)
{
    if (value.empty())
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::invalid_content_length, offset});
    }
    std::size_t length = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), length);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size())
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::invalid_content_length, offset});
    }
    return length;
}

/// @brief Parses the method, origin-form target, and HTTP version from one request line.
/// @param[in] line Request line excluding its CRLF terminator.
/// @param[out] target Borrowed validated request target.
/// @return Supported method or a structured request-line failure.
[[nodiscard]] Result<HttpMethod, HttpRequestParseError>
parse_request_line(const std::string_view line, std::string_view &target)
{
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos)
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::malformed_request_line, 0});
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos)
    {
        return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::malformed_request_line,
                                                first_space + 1});
    }

    auto method_result = parse_method(line.substr(0, first_space), 0);
    if (!method_result)
    {
        return unexpected(method_result.error());
    }
    target = line.substr(first_space + 1, second_space - first_space - 1);
    if (!valid_request_target(target))
    {
        return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::invalid_request_target,
                                                first_space + 1});
    }
    if (line.substr(second_space + 1) != "HTTP/1.1")
    {
        return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::unsupported_http_version,
                                                second_space + 1});
    }
    return method_result;
}

/// @brief Removes HTTP optional whitespace from both ends of a header value.
/// @param[in] value Untrimmed field value.
/// @return Borrowed view containing no leading or trailing spaces or horizontal tabs.
[[nodiscard]] std::string_view trim_optional_whitespace(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

/// @brief Rejects control bytes forbidden inside a parsed HTTP field value.
/// @param[in] value Trimmed header value to validate.
/// @return `true` when the value contains only permitted bytes.
[[nodiscard]] bool valid_header_value(const std::string_view value) noexcept
{
    return std::ranges::none_of(value, [](const unsigned char byte)
                                { return (byte < 0x20U && byte != '\t') || byte == 0x7FU; });
}

/// @brief Validates and separates one non-empty HTTP header line.
/// @param[in] line Header line excluding its CRLF terminator.
/// @param[in] line_start Source offset at which the header line begins.
/// @return Borrowed field views and diagnostic offsets, or a syntax error.
[[nodiscard]] Result<ParsedHeaderField, HttpRequestParseError>
parse_header_field(const std::string_view line, const std::size_t line_start)
{
    if (line.front() == ' ' || line.front() == '\t')
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::folded_header, line_start});
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0)
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::malformed_header, line_start});
    }
    const std::string_view name = line.substr(0, colon);
    if (!std::ranges::all_of(name, is_token_character))
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::malformed_header, line_start});
    }
    const std::string_view untrimmed_value = line.substr(colon + 1);
    const std::size_t leading_whitespace = untrimmed_value.find_first_not_of(" \t");
    const std::string_view value = trim_optional_whitespace(untrimmed_value);
    if (!valid_header_value(value))
    {
        return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::malformed_header,
                                                line_start + colon + 1});
    }
    const std::size_t value_start =
        line_start + colon + 1 +
        (leading_whitespace == std::string_view::npos ? untrimmed_value.size()
                                                      : leading_whitespace);
    return ParsedHeaderField{{name, value}, line_start, value_start};
}

/// @brief Applies Host and message-framing rules to one syntactically valid field.
/// @param[in] parsed Validated header field and its diagnostic offsets.
/// @param[in] limits Configured request body boundary.
/// @param[in,out] state Accumulated Host, Content-Length, and header state.
/// @return Success or a structured semantic/framing failure.
[[nodiscard]] Result<void, HttpRequestParseError>
apply_header_field(const ParsedHeaderField &parsed, const HttpRequestParserLimits &limits,
                   HeaderParseState &state)
{
    const HttpHeaderView &field = parsed.field;
    if (ascii_case_insensitive_equal(field.name, "Host"))
    {
        if (state.host_seen)
        {
            return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::duplicate_host,
                                                    parsed.line_start});
        }
        if (field.value.empty())
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::missing_host, parsed.value_start});
        }
        if (!detail::is_valid_host_authority(field.value))
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::invalid_host, parsed.value_start});
        }
        state.host_seen = true;
    }
    else if (ascii_case_insensitive_equal(field.name, "Content-Length"))
    {
        if (state.body_size.has_value())
        {
            return unexpected(HttpRequestParseError{
                HttpRequestParseErrorCode::duplicate_content_length, parsed.line_start});
        }
        auto length_result = parse_content_length(field.value, parsed.value_start);
        if (!length_result)
        {
            return unexpected(length_result.error());
        }
        if (length_result.value() > limits.max_body_bytes)
        {
            return unexpected(HttpRequestParseError{HttpRequestParseErrorCode::body_too_large,
                                                    parsed.value_start});
        }
        state.body_size = length_result.value();
    }
    else if (ascii_case_insensitive_equal(field.name, "Transfer-Encoding"))
    {
        return unexpected(HttpRequestParseError{
            HttpRequestParseErrorCode::unsupported_transfer_encoding, parsed.line_start});
    }
    state.headers.push_back(field);
    return {};
}

/// @brief Parses and validates the complete header section when it is available.
/// @param[in] input Complete caller-provided byte view.
/// @param[in] header_start Offset immediately after the request-line CRLF.
/// @param[in] limits Explicit header, count, and body boundaries.
/// @return Parsed header state, no value for incomplete input, or a protocol error.
[[nodiscard]] Result<std::optional<ParsedHeaderSection>, HttpRequestParseError>
parse_header_section(const std::string_view input, const std::size_t header_start,
                     const HttpRequestParserLimits &limits)
{
    std::size_t line_start = header_start;
    HeaderParseState state;
    while (true)
    {
        const HeaderLineSearch search{line_start, header_start, limits.max_header_bytes};
        auto line_end_result = find_header_line_end(input, search);
        if (!line_end_result)
        {
            return unexpected(line_end_result.error());
        }
        if (!line_end_result->has_value())
        {
            return std::optional<ParsedHeaderSection>{};
        }
        const std::size_t line_end = line_end_result->value_or(LineEnd{}).offset;
        if (line_end == line_start)
        {
            if (!state.host_seen)
            {
                return unexpected(
                    HttpRequestParseError{HttpRequestParseErrorCode::missing_host, header_start});
            }
            return std::optional<ParsedHeaderSection>{ParsedHeaderSection{
                std::move(state.headers), line_end + 2, state.body_size.value_or(0)}};
        }
        if (state.headers.size() >= limits.max_header_count)
        {
            return unexpected(
                HttpRequestParseError{HttpRequestParseErrorCode::too_many_headers, line_start});
        }
        auto field_result =
            parse_header_field(input.substr(line_start, line_end - line_start), line_start);
        if (!field_result)
        {
            return unexpected(field_result.error());
        }
        auto apply_result = apply_header_field(field_result.value(), limits, state);
        if (!apply_result)
        {
            return unexpected(apply_result.error());
        }
        line_start = line_end + 2;
    }
}

} // namespace

/// @brief Parses one complete bounded HTTP/1.1 request when enough bytes are available.
Result<HttpRequestParseResult, HttpRequestParseError>
parse_http_request(const std::span<const std::byte> input, const HttpRequestParserLimits &limits)
{
    if (input.empty())
    {
        return HttpRequestParseResult::incomplete();
    }
    const std::string_view text(reinterpret_cast<const char *>(input.data()), input.size());
    auto request_line_end_result = find_request_line_end(text, limits.max_request_line_bytes);
    if (!request_line_end_result)
    {
        return unexpected(request_line_end_result.error());
    }
    if (!request_line_end_result->has_value())
    {
        return HttpRequestParseResult::incomplete();
    }

    const std::size_t request_line_end = request_line_end_result->value_or(LineEnd{}).offset;
    std::string_view target;
    auto method_result = parse_request_line(text.substr(0, request_line_end), target);
    if (!method_result)
    {
        return unexpected(method_result.error());
    }

    const std::size_t header_start = request_line_end + 2;
    auto header_result = parse_header_section(text, header_start, limits);
    if (!header_result)
    {
        return unexpected(header_result.error());
    }
    if (!header_result->has_value())
    {
        return HttpRequestParseResult::incomplete();
    }
    ParsedHeaderSection parsed_headers = header_result->value_or(ParsedHeaderSection{});
    if (input.size() - parsed_headers.body_start < parsed_headers.body_size)
    {
        return HttpRequestParseResult::incomplete();
    }

    return HttpRequestParseResult::complete(
        parsed_headers.body_start + parsed_headers.body_size,
        detail::HttpRequestViewAccess::create(
            method_result.value(), target, std::move(parsed_headers.headers),
            input.subspan(parsed_headers.body_start, parsed_headers.body_size)));
}

/// @brief Converts a portable HTTP parse failure into stable diagnostic text.
const char *to_string(const HttpRequestParseErrorCode code) noexcept
{
    switch (code)
    {
    case HttpRequestParseErrorCode::request_line_too_large:
        return "HTTP request line exceeds its configured limit";
    case HttpRequestParseErrorCode::headers_too_large:
        return "HTTP headers exceed their configured limit";
    case HttpRequestParseErrorCode::too_many_headers:
        return "HTTP request contains too many headers";
    case HttpRequestParseErrorCode::body_too_large:
        return "HTTP request body exceeds its configured limit";
    case HttpRequestParseErrorCode::invalid_line_ending:
        return "HTTP protocol lines must end with CRLF";
    case HttpRequestParseErrorCode::malformed_request_line:
        return "HTTP request line is malformed";
    case HttpRequestParseErrorCode::invalid_method:
        return "HTTP method is not a valid token";
    case HttpRequestParseErrorCode::unsupported_method:
        return "HTTP method is not supported";
    case HttpRequestParseErrorCode::invalid_request_target:
        return "HTTP request target is invalid";
    case HttpRequestParseErrorCode::unsupported_http_version:
        return "HTTP version is not supported";
    case HttpRequestParseErrorCode::malformed_header:
        return "HTTP header is malformed";
    case HttpRequestParseErrorCode::folded_header:
        return "folded HTTP headers are not supported";
    case HttpRequestParseErrorCode::missing_host:
        return "HTTP/1.1 Host header is missing";
    case HttpRequestParseErrorCode::duplicate_host:
        return "HTTP/1.1 Host header is repeated";
    case HttpRequestParseErrorCode::invalid_host:
        return "HTTP/1.1 Host header is invalid";
    case HttpRequestParseErrorCode::invalid_content_length:
        return "HTTP Content-Length is invalid";
    case HttpRequestParseErrorCode::duplicate_content_length:
        return "HTTP Content-Length is repeated";
    case HttpRequestParseErrorCode::unsupported_transfer_encoding:
        return "HTTP Transfer-Encoding is not supported";
    }
    return "unknown HTTP request parse error";
}

} // namespace sparenode::http
