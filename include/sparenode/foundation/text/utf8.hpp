#pragma once

#include <string>
#include <string_view>

namespace sparenode::text::utf8
{

/// @brief Checks that every byte belongs to one canonical Unicode scalar sequence.
///
/// Validation covers the complete UTF-8 contract: valid leading and continuation bytes, complete
/// sequences, shortest-form encoding, exclusion of UTF-16 surrogates, and the U+10FFFF limit.
/// @param[in] input Complete byte sequence to validate, including any embedded null bytes.
/// @return True only for well-formed UTF-8 without overlong encodings or surrogates.
[[nodiscard]] bool is_valid(std::string_view input) noexcept;

/// @brief Appends one Unicode scalar value using its canonical UTF-8 representation.
///
/// The function chooses the shortest one-, two-, three-, or four-byte sequence for the value. It
/// validates the value before modifying the destination, so a false result leaves output intact.
/// @param[in,out] output Destination that receives one through four encoded bytes.
/// @param[in] code_point Unicode scalar value to encode.
/// @return False for a surrogate or a value above U+10FFFF.
/// @throws std::bad_alloc when the destination cannot grow.
[[nodiscard]] bool append_code_point(std::string &output, char32_t code_point);

} // namespace sparenode::text::utf8
