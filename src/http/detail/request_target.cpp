#include "sparenode/http/detail/request_target.hpp"
#include "sparenode/http/detail/host_authority.hpp"

#include <algorithm>

namespace sparenode::http::detail
{
namespace
{
/// @brief Width of one percent escape in a URI.
constexpr std::size_t percent_encoded_sequence_size = 3;

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

} // namespace
/// @brief Separates supported absolute targets and preserves validated path/query bytes.
Result<ParsedRequestTarget, HttpRequestParseError>
parse_request_target(const std::string_view target, const HttpMethod method,
                     const std::size_t offset)
{
    const auto invalid =
        HttpRequestParseError{HttpRequestParseErrorCode::invalid_request_target, offset};
    if (target == "*")
    {
        if (method != HttpMethod::options)
        {
            return unexpected(invalid);
        }
        return ParsedRequestTarget{target};
    }
    if (target.starts_with('/'))
    {
        return valid_request_target(target)
                   ? Result<ParsedRequestTarget, HttpRequestParseError>{ParsedRequestTarget{target}}
                   : unexpected(invalid);
    }
    // Only the cleartext HTTP origin scheme is supported by this server profile.
    if (target.size() < 7 || (target[0] != 'h' && target[0] != 'H') ||
        (target[1] != 't' && target[1] != 'T') || (target[2] != 't' && target[2] != 'T') ||
        (target[3] != 'p' && target[3] != 'P') || target.substr(4, 3) != "://")
    {
        return unexpected(invalid);
    }
    const auto absolute = target.substr(7);
    const auto path_start = absolute.find_first_of("/?#");
    const auto authority = absolute.substr(0, path_start);
    if (!is_valid_host_authority(authority))
    {
        return unexpected(invalid);
    }
    ParsedRequestTarget result{.routing_target = "/", .authority = authority};
    if (path_start == std::string_view::npos)
    {
        return result;
    }
    const auto path = absolute.substr(path_start);
    if (path.starts_with('?'))
    {
        result.storage = std::make_shared<const std::string>("/" + std::string(path));
        result.routing_target = *result.storage;
    }
    else
    {
        result.routing_target = path;
    }
    if (!valid_request_target(result.routing_target))
    {
        return unexpected(invalid);
    }
    return result;
}

/// @brief Accepts supported single-digit HTTP/1.x versions without changing response version.
Result<void, HttpRequestParseError> validate_request_version(const std::string_view version,
                                                             const std::size_t offset) noexcept
{
    constexpr std::size_t version_bytes = 8;
    if (version.size() != version_bytes || !version.starts_with("HTTP/") || version[6] != '.' ||
        version[5] < '0' || version[5] > '9' || version[7] < '0' || version[7] > '9')
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::malformed_http_version, offset});
    }
    if (version[5] != '1' || version[7] == '0')
    {
        return unexpected(
            HttpRequestParseError{HttpRequestParseErrorCode::unsupported_http_version, offset});
    }
    return {};
}
} // namespace sparenode::http::detail
