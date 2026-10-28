#include "sparenode/foundation/text/numeric_ip_address.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string_view>

#include "sparenode/foundation/text/hexadecimal.hpp"

namespace sparenode::text::ip
{
namespace
{

/// @brief Parses exactly four canonical dotted-decimal IPv4 octets.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>>
parse_ipv4(const std::string_view value) noexcept
{
    std::array<std::uint8_t, 4> bytes{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        const auto end = value.find('.', begin);
        const auto component_end = end == std::string_view::npos ? value.size() : end;
        const auto component = value.substr(begin, component_end - begin);
        if (component.empty() || component.size() > 3 ||
            (component.size() > 1 && component.front() == '0'))
        {
            return std::nullopt;
        }
        unsigned int number = 0;
        for (const char digit : component)
        {
            if (digit < '0' || digit > '9')
            {
                return std::nullopt;
            }
            // Shift the accumulated value by one base-10 digit before appending the next digit.
            number = number * 10U + static_cast<unsigned int>(digit - '0');
        }
        if (number > std::numeric_limits<std::uint8_t>::max())
        {
            return std::nullopt;
        }
        bytes[index] = static_cast<std::uint8_t>(number);
        if (index + 1 < bytes.size())
        {
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            begin = end + 1;
        }
        else if (end != std::string_view::npos)
        {
            return std::nullopt;
        }
    }
    return bytes;
}

/// @brief Parses one bounded hexadecimal IPv6 word.
[[nodiscard]] std::optional<std::uint16_t> parse_ipv6_word(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > 4)
    {
        return std::nullopt;
    }
    std::uint16_t word = 0;
    for (const char digit : value)
    {
        const auto nibble = hexadecimal::digit_value(digit);
        if (!nibble)
        {
            return std::nullopt;
        }
        // Shift by one four-bit hexadecimal digit before appending the next nibble.
        word = static_cast<std::uint16_t>((word << 4U) | *nibble);
    }
    return word;
}

/// @brief Reports whether an IPv6 value carries the RFC IPv4-mapped prefix.
[[nodiscard]] bool is_ipv4_mapped_address(const NumericAddress &address) noexcept
{
    constexpr std::size_t zero_prefix_bytes = 10;
    constexpr std::size_t marker_first_index = 10;
    constexpr std::size_t marker_second_index = 11;
    constexpr std::uint8_t marker_byte = 0xFFU;
    return !address.ipv4 &&
           std::ranges::all_of(address.bytes.begin(), address.bytes.begin() + zero_prefix_bytes,
                               [](const std::uint8_t byte) { return byte == 0; }) &&
           address.bytes[marker_first_index] == marker_byte &&
           address.bytes[marker_second_index] == marker_byte;
}

/// @brief Appends IPv6 words from one side of an optional compression marker.
[[nodiscard]] bool append_ipv6_words(const std::string_view sequence,
                                     std::array<std::uint16_t, 8> &words, std::size_t &word_count,
                                     const bool allow_ipv4_tail) noexcept
{
    if (sequence.empty())
    {
        return true;
    }
    std::size_t begin = 0;
    while (begin <= sequence.size())
    {
        const auto end = sequence.find(':', begin);
        const auto component_end = end == std::string_view::npos ? sequence.size() : end;
        const auto component = sequence.substr(begin, component_end - begin);
        if (component.contains('.'))
        {
            if (!allow_ipv4_tail || end != std::string_view::npos)
            {
                return false;
            }
            const auto ipv4 = parse_ipv4(component);
            if (!ipv4)
            {
                return false;
            }
            if (word_count > words.size() - 2)
            {
                return false;
            }
            // Move the first IPv4 octet into the high byte of the first IPv6 word.
            words[word_count++] = static_cast<std::uint16_t>(((*ipv4)[0] << 8U) | (*ipv4)[1]);
            // Move the third IPv4 octet into the high byte of the second IPv6 word.
            words[word_count++] = static_cast<std::uint16_t>(((*ipv4)[2] << 8U) | (*ipv4)[3]);
            return true;
        }
        const auto word = parse_ipv6_word(component);
        if (!word)
        {
            return false;
        }
        if (word_count == words.size())
        {
            return false;
        }
        words[word_count++] = *word;
        if (end == std::string_view::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return true;
}

} // namespace

std::optional<NumericAddress> parse_numeric_address(const std::string_view value) noexcept
{
    if (const auto ipv4 = parse_ipv4(value))
    {
        NumericAddress result;
        result.ipv4 = true;
        std::ranges::copy(*ipv4, result.bytes.begin());
        return result;
    }

    const auto compression = value.find("::");
    if (compression != std::string_view::npos &&
        value.find("::", compression + 2) != std::string_view::npos)
    {
        return std::nullopt;
    }
    const bool compressed = compression != std::string_view::npos;
    const auto left_text = compressed ? value.substr(0, compression) : value;
    const auto right_text = compressed ? value.substr(compression + 2) : std::string_view{};
    std::array<std::uint16_t, 8> left{};
    std::array<std::uint16_t, 8> right{};
    std::size_t left_count = 0;
    std::size_t right_count = 0;
    if (!append_ipv6_words(left_text, left, left_count, !compressed) ||
        !append_ipv6_words(right_text, right, right_count, true))
    {
        return std::nullopt;
    }
    constexpr std::size_t ipv6_word_count = 8;
    const auto word_count = left_count + right_count;
    if ((!compressed && word_count != ipv6_word_count) ||
        (compressed && word_count >= ipv6_word_count))
    {
        return std::nullopt;
    }

    NumericAddress result;
    const auto store_word = [&result](const std::size_t index, const std::uint16_t word)
    {
        // Shift one byte right to extract the IPv6 word's high-order byte.
        result.bytes[index * 2] = static_cast<std::uint8_t>(word >> 8U);
        // Mask off the high byte to extract the IPv6 word's low-order eight bits.
        result.bytes[index * 2 + 1] = static_cast<std::uint8_t>(word & 0xFFU);
    };
    for (std::size_t index = 0; index < left_count; ++index)
    {
        store_word(index, left[index]);
    }
    for (std::size_t index = 0; index < right_count; ++index)
    {
        store_word(ipv6_word_count - right_count + index, right[index]);
    }
    return result;
}

bool is_valid_ipv4_address(const std::string_view value) noexcept
{
    return parse_ipv4(value).has_value();
}

bool is_valid_ipv6_address(const std::string_view value) noexcept
{
    const auto parsed = parse_numeric_address(value);
    return parsed.has_value() && !parsed->ipv4;
}

bool is_loopback_address(const NumericAddress &address) noexcept
{
    if (address.ipv4)
    {
        return address.bytes.front() == 127;
    }
    return std::ranges::all_of(address.bytes.begin(), address.bytes.end() - 1,
                               [](const std::uint8_t byte) { return byte == 0; }) &&
           address.bytes.back() == 1;
}

bool is_unspecified_address(const NumericAddress &address) noexcept
{
    constexpr std::size_t ipv4_byte_count = 4;
    constexpr std::size_t mapped_ipv4_offset = 12;
    const bool ipv4_mapped = is_ipv4_mapped_address(address);
    const auto begin =
        ipv4_mapped ? address.bytes.begin() + mapped_ipv4_offset : address.bytes.begin();
    const auto size = address.ipv4 || ipv4_mapped ? ipv4_byte_count : address.bytes.size();
    return std::ranges::all_of(begin, begin + size,
                               [](const std::uint8_t byte) { return byte == 0; });
}

} // namespace sparenode::text::ip
