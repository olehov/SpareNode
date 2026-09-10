#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace sparenode::http::detail
{
/// @brief Serialized Date field size, including its name, separator and CRLF.
inline constexpr std::size_t response_date_field_bytes = 37;
/// @brief Formats a UTC system-clock instant as locale-independent IMF-fixdate.
/// @param[in] instant Message origination time; year must fit four decimal digits.
/// @return A 29-byte HTTP date; throws for an unrepresentable calendar year.
[[nodiscard]] std::string format_response_date(std::chrono::system_clock::time_point instant);
} // namespace sparenode::http::detail
