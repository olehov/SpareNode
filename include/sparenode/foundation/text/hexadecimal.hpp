#pragma once

#include <cstdint>
#include <optional>

namespace sparenode::text::hexadecimal
{

/// @brief Converts one ASCII hexadecimal digit to its numeric value.
/// @param[in] character Byte to classify without locale-sensitive rules.
/// @return Value from zero to fifteen, or no value for a non-hexadecimal byte.
[[nodiscard]] constexpr std::optional<std::uint8_t> digit_value(const char character) noexcept
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'A' && character <= 'F')
    {
        return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    if (character >= 'a' && character <= 'f')
    {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    return std::nullopt;
}

} // namespace sparenode::text::hexadecimal
