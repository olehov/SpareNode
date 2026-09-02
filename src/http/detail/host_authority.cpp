#include "sparenode/http/detail/host_authority.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sparenode::http::detail
{
namespace
{

constexpr std::size_t maximum_hostname_bytes = 253;
constexpr std::size_t maximum_hostname_label_bytes = 63;
constexpr std::uint32_t maximum_port = 65535;
constexpr std::uint32_t maximum_ipv4_octet = 255;
constexpr std::size_t ipv6_group_count = 8;
constexpr std::size_t ipv4_ipv6_group_count = 2;

/// @brief Checks one locale-independent ASCII decimal byte.
[[nodiscard]] constexpr bool is_ascii_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/// @brief Checks one locale-independent ASCII hexadecimal byte.
[[nodiscard]] constexpr bool is_ascii_hex_digit(const char byte) noexcept
{
    return is_ascii_digit(byte) || (byte >= 'A' && byte <= 'F') || (byte >= 'a' && byte <= 'f');
}

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

/// @brief Validates strict four-octet dotted-decimal IPv4 text.
[[nodiscard]] bool is_valid_ipv4(const std::string_view address) noexcept
{
    if (address.empty() || address.front() == '.' || address.back() == '.')
    {
        return false;
    }
    std::size_t octet_start = 0;
    std::size_t octet_count = 0;
    while (octet_start < address.size())
    {
        const std::size_t dot = address.find('.', octet_start);
        const std::size_t octet_end = dot == std::string_view::npos ? address.size() : dot;
        const std::string_view octet = address.substr(octet_start, octet_end - octet_start);
        std::uint32_t value = 0;
        if (octet.size() > 1 && octet.front() == '0')
        {
            return false;
        }
        if (!parse_decimal(octet, maximum_ipv4_octet, value))
        {
            return false;
        }
        ++octet_count;
        if (dot == std::string_view::npos)
        {
            break;
        }
        octet_start = dot + 1;
    }
    return octet_count == 4;
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

/// @brief Returns the IPv6 slot width of one valid hexadecimal or embedded-IPv4 group.
[[nodiscard]] std::size_t ipv6_group_width(const std::string_view group, const bool may_be_ipv4,
                                           const bool is_last_group) noexcept
{
    if (group.empty())
    {
        return 0;
    }
    if (group.contains('.'))
    {
        return may_be_ipv4 && is_last_group && is_valid_ipv4(group) ? ipv4_ipv6_group_count : 0;
    }
    if (group.size() > 4 || !std::ranges::all_of(group, is_ascii_hex_digit))
    {
        return 0;
    }
    return 1;
}

/// @brief Counts validated IPv6 groups on one side of an optional compression marker.
[[nodiscard]] bool count_ipv6_groups(const std::string_view side, const bool may_end_with_ipv4,
                                     std::size_t &group_count) noexcept
{
    if (side.empty())
    {
        return true;
    }
    std::size_t group_start = 0;
    while (group_start < side.size())
    {
        const std::size_t colon = side.find(':', group_start);
        const std::size_t group_end = colon == std::string_view::npos ? side.size() : colon;
        const std::string_view group = side.substr(group_start, group_end - group_start);
        const std::size_t width =
            ipv6_group_width(group, may_end_with_ipv4, colon == std::string_view::npos);
        if (width == 0)
        {
            return false;
        }
        group_count += width;
        if (colon == std::string_view::npos)
        {
            return true;
        }
        group_start = colon + 1;
    }
    return false;
}

/// @brief Validates an IPv6 literal without its required HTTP brackets.
[[nodiscard]] bool is_valid_ipv6(const std::string_view address) noexcept
{
    if (address.empty())
    {
        return false;
    }
    const std::size_t compression = address.find("::");
    const bool compressed = compression != std::string_view::npos;
    if (compressed && address.find("::", compression + 2) != std::string_view::npos)
    {
        return false;
    }

    const std::string_view left = compressed ? address.substr(0, compression) : address;
    const std::string_view right =
        compressed ? address.substr(compression + 2) : std::string_view{};
    std::size_t group_count = 0;
    if (!count_ipv6_groups(left, !compressed, group_count) ||
        !count_ipv6_groups(right, true, group_count))
    {
        return false;
    }
    return compressed ? group_count < ipv6_group_count : group_count == ipv6_group_count;
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
        !is_valid_ipv6(authority.substr(1, closing_bracket - 1)))
    {
        return false;
    }
    const std::string_view remainder = authority.substr(closing_bracket + 1);
    return remainder.empty() || (remainder.starts_with(':') && is_valid_port(remainder.substr(1)));
}

} // namespace

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
    return looks_like_ipv4(host) ? is_valid_ipv4(host) : is_valid_hostname(host);
}

} // namespace sparenode::http::detail
