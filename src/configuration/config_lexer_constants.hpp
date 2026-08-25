#pragma once

#include <array>
#include <cstdint>

namespace sparenode::configuration::config_lexer_constants
{

/// @brief Byte sequence that may prefix a UTF-8 configuration file.
inline constexpr std::array<unsigned char, 3> utf8_byte_order_mark{0xEFU, 0xBBU, 0xBFU};

/// @brief Selects the prefix bits of a two-byte UTF-8 leading byte.
inline constexpr unsigned char utf8_two_byte_prefix_mask = 0xE0U;

/// @brief Expected prefix bits of a two-byte UTF-8 leading byte (`110xxxxx`).
inline constexpr unsigned char utf8_two_byte_prefix = 0xC0U;

/// @brief Selects the code-point payload of a two-byte UTF-8 leading byte.
inline constexpr unsigned char utf8_two_byte_payload_mask = 0x1FU;

/// @brief Selects the prefix bits of a three-byte UTF-8 leading byte.
inline constexpr unsigned char utf8_three_byte_prefix_mask = 0xF0U;

/// @brief Expected prefix bits of a three-byte UTF-8 leading byte (`1110xxxx`).
inline constexpr unsigned char utf8_three_byte_prefix = 0xE0U;

/// @brief Selects the code-point payload of a three-byte UTF-8 leading byte.
inline constexpr unsigned char utf8_three_byte_payload_mask = 0x0FU;

/// @brief Selects the prefix bits of a four-byte UTF-8 leading byte.
inline constexpr unsigned char utf8_four_byte_prefix_mask = 0xF8U;

/// @brief Expected prefix bits of a four-byte UTF-8 leading byte (`11110xxx`).
inline constexpr unsigned char utf8_four_byte_prefix = 0xF0U;

/// @brief Selects the code-point payload of a four-byte UTF-8 leading byte.
inline constexpr unsigned char utf8_four_byte_payload_mask = 0x07U;

/// @brief Selects the prefix bits of a UTF-8 continuation byte.
inline constexpr unsigned char utf8_continuation_prefix_mask = 0xC0U;

/// @brief Expected prefix bits of a UTF-8 continuation byte (`10xxxxxx`).
inline constexpr unsigned char utf8_continuation_prefix = 0x80U;

/// @brief Selects the six code-point payload bits of a UTF-8 continuation byte.
inline constexpr unsigned char utf8_continuation_payload_mask = 0x3FU;

/// @brief Smallest byte value that does not represent an ASCII character.
inline constexpr unsigned char utf8_first_non_ascii_byte = 0x80U;

/// @brief Smallest code point that requires a two-byte UTF-8 sequence.
inline constexpr std::uint32_t utf8_two_byte_minimum_code_point = 0x80U;

/// @brief Smallest code point that requires a three-byte UTF-8 sequence.
inline constexpr std::uint32_t utf8_three_byte_minimum_code_point = 0x800U;

/// @brief Smallest code point that requires a four-byte UTF-8 sequence.
inline constexpr std::uint32_t utf8_four_byte_minimum_code_point = 0x10000U;

/// @brief First UTF-16 surrogate value, which UTF-8 must reject.
inline constexpr std::uint32_t unicode_first_surrogate = 0xD800U;

/// @brief Last UTF-16 surrogate value, which UTF-8 must reject.
inline constexpr std::uint32_t unicode_last_surrogate = 0xDFFFU;

/// @brief Largest valid Unicode scalar value.
inline constexpr std::uint32_t unicode_maximum_code_point = 0x10FFFFU;

/// @brief Number of code-point payload bits carried by each continuation byte.
inline constexpr std::uint32_t utf8_continuation_payload_bits = 6U;

} // namespace sparenode::configuration::config_lexer_constants
