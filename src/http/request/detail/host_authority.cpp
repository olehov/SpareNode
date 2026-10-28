#include "sparenode/http/request/detail/host_authority.hpp"

#include "sparenode/http/detail/ascii.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "sparenode/foundation/text/numeric_ip_address.hpp"

namespace sparenode::http::detail
{
namespace
{

constexpr std::size_t maximum_hostname_bytes = 253;
constexpr std::size_t maximum_hostname_label_bytes = 63;
constexpr std::uint32_t maximum_port = 65535;

/// @brief Checks one ASCII byte allowed at a DNS label boundary.
[[nodiscard]] constexpr bool is_hostname_boundary(const char byte) noexcept
{
    return is_ascii_digit(byte) || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

/// @brief Checks one ASCII byte allowed anywhere inside a DNS label.
[[nodiscard]] constexpr bool is_hostname_character(const char byte) noexcept
{
    return is_hostname_boundary(byte) || byte == '-';
}

/// @brief Parses a non-empty bounded decimal component.
[[nodiscard]] bool parse_decimal(const std::string_view text, const std::uint32_t maximum,
                                 std::uint32_t &value) noexcept
{
    if (text.empty())
    {
        return false;
    }
    value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= maximum;
}

/// @brief Validates a DNS-style hostname with an optional terminal root dot.
[[nodiscard]] bool is_valid_hostname(std::string_view hostname) noexcept
{
    if (hostname.empty())
    {
        return false;
    }
    if (hostname.back() == '.')
    {
        hostname.remove_suffix(1);
        if (hostname.empty() || hostname.back() == '.')
        {
            return false;
        }
    }
    if (hostname.size() > maximum_hostname_bytes)
    {
        return false;
    }

    std::size_t label_start = 0;
    while (label_start < hostname.size())
    {
        const std::size_t dot = hostname.find('.', label_start);
        const std::size_t label_end = dot == std::string_view::npos ? hostname.size() : dot;
        const std::string_view label = hostname.substr(label_start, label_end - label_start);
        if (label.empty() || label.size() > maximum_hostname_label_bytes ||
            !is_hostname_boundary(label.front()) || !is_hostname_boundary(label.back()))
        {
            return false;
        }
        if (!std::ranges::all_of(label, is_hostname_character))
        {
            return false;
        }
        if (dot == std::string_view::npos)
        {
            return true;
        }
        label_start = dot + 1;
    }
    return true;
}

/// @brief Reports whether a dotted candidate must be treated as numeric IPv4.
[[nodiscard]] bool looks_like_ipv4(const std::string_view host) noexcept
{
    if (!host.contains('.'))
    {
        return false;
    }
    return std::ranges::all_of(host,
                               [](const char byte) { return is_ascii_digit(byte) || byte == '.'; });
}

/// @brief Validates an optional decimal TCP port representable by 16 bits.
[[nodiscard]] bool is_valid_port(const std::string_view port) noexcept
{
    std::uint32_t value = 0;
    return parse_decimal(port, maximum_port, value);
}

/// @brief Validates a bracketed IPv6 authority and its optional port.
[[nodiscard]] bool is_valid_ip_literal_authority(const std::string_view authority) noexcept
{
    const std::size_t closing_bracket = authority.find(']');
    if (closing_bracket == std::string_view::npos || closing_bracket == 1 ||
        !text::ip::is_valid_ipv6_address(authority.substr(1, closing_bracket - 1)))
    {
        return false;
    }
    const std::string_view remainder = authority.substr(closing_bracket + 1);
    return remainder.empty() || (remainder.starts_with(':') && is_valid_port(remainder.substr(1)));
}

} // namespace

/// @brief Validates host or IP authority syntax with an optional numeric port.
bool is_valid_host_authority(const std::string_view authority) noexcept
{
    if (authority.empty())
    {
        return false;
    }
    if (authority.front() == '[')
    {
        return is_valid_ip_literal_authority(authority);
    }
    if (authority.contains('[') || authority.contains(']'))
    {
        return false;
    }

    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos && authority.find(':', colon + 1) != std::string_view::npos)
    {
        return false;
    }
    const std::string_view host = authority.substr(0, colon);
    const std::string_view port =
        colon == std::string_view::npos ? std::string_view{} : authority.substr(colon + 1);
    if (host.empty() || (colon != std::string_view::npos && !is_valid_port(port)))
    {
        return false;
    }
    return looks_like_ipv4(host) ? text::ip::is_valid_ipv4_address(host) : is_valid_hostname(host);
}

} // namespace sparenode::http::detail
