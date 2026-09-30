#pragma once

#include <string_view>

namespace sparenode::http::detail
{

/// @brief Folds one uppercase ASCII letter without consulting the process locale.
/// @param[in] byte Byte to normalize.
/// @return Lowercase ASCII letter or the unchanged non-uppercase byte.
[[nodiscard]] constexpr char ascii_lower(const char byte) noexcept
{
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte;
}

/// @brief Compares two ASCII protocol values without locale-dependent case conversion.
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
        if (ascii_lower(left[index]) != ascii_lower(right[index]))
        {
            return false;
        }
    }
    return true;
}

/// @brief Checks whether one byte is an ASCII decimal digit.
/// @param[in] byte Byte to classify.
/// @return `true` for bytes from `0` through `9`.
[[nodiscard]] constexpr bool is_ascii_digit(const unsigned char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/// @brief Checks whether one byte is an ASCII hexadecimal digit.
/// @param[in] byte Byte to classify.
/// @return `true` for decimal digits or ASCII letters A-F in either case.
[[nodiscard]] constexpr bool is_ascii_hexadecimal_digit(const unsigned char byte) noexcept
{
    return is_ascii_digit(byte) || (byte >= 'A' && byte <= 'F') || (byte >= 'a' && byte <= 'f');
}

/// @brief Checks whether one byte is permitted in an HTTP token.
/// @param[in] byte Byte to classify.
/// @return `true` for an RFC token character.
[[nodiscard]] constexpr bool is_http_token_character(const char byte) noexcept
{
    if (is_ascii_digit(static_cast<unsigned char>(byte)) || (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z'))
    {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.find(byte) != std::string_view::npos;
}

} // namespace sparenode::http::detail
