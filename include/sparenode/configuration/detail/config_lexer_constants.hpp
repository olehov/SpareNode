#pragma once

#include <array>
#include <cstddef>

namespace sparenode::configuration::detail::config_lexer_constants
{

/// @brief Byte sequence that may prefix a UTF-8 configuration file.
inline constexpr std::array<unsigned char, 3> utf8_byte_order_mark{0xEFU, 0xBBU, 0xBFU};

/// @brief Largest diagnostic context, including an escape prefix and one UTF-8 scalar.
inline constexpr std::size_t maximum_error_context_bytes = 5;

} // namespace sparenode::configuration::detail::config_lexer_constants
