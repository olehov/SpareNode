#pragma once

#include <cstddef>
#include <cstdint>

namespace sparenode::http::detail
{

/// @brief Counts decimal digits in one unsigned HTTP framing value.
/// @param[in] value Value that will be serialized in base ten.
/// @return Number of decimal digits, including one digit for zero.
[[nodiscard]] constexpr std::size_t decimal_digit_count(std::uint64_t value) noexcept
{
    std::size_t count = 1;
    while (value >= 10)
    {
        value /= 10;
        ++count;
    }
    return count;
}

} // namespace sparenode::http::detail
