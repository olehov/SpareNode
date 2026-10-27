#include "sparenode/foundation/text/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace sparenode::text::utf8
{
namespace
{

// UTF-8 stores ASCII directly in one byte. Larger code points use one leading byte followed by
// one to three continuation bytes. The leading-byte masks below both identify the sequence length
// and remove the fixed prefix bits, leaving only the code-point payload.
constexpr std::uint32_t ascii_limit = 0x80U;

constexpr std::uint32_t two_byte_leading_mask = 0xE0U;
constexpr std::uint32_t two_byte_leading_pattern = 0xC0U;
constexpr std::uint32_t two_byte_leading_payload_mask = 0x1FU;
constexpr std::size_t two_byte_sequence_length = 2;
constexpr std::uint32_t two_byte_minimum_code_point = 0x80U;

constexpr std::uint32_t three_byte_leading_mask = 0xF0U;
constexpr std::uint32_t three_byte_leading_pattern = 0xE0U;
constexpr std::uint32_t three_byte_leading_payload_mask = 0x0FU;
constexpr std::size_t three_byte_sequence_length = 3;
constexpr std::uint32_t three_byte_minimum_code_point = 0x800U;

constexpr std::uint32_t four_byte_leading_mask = 0xF8U;
constexpr std::uint32_t four_byte_leading_pattern = 0xF0U;
constexpr std::uint32_t four_byte_leading_payload_mask = 0x07U;
constexpr std::size_t four_byte_sequence_length = 4;
constexpr std::uint32_t four_byte_minimum_code_point = 0x10000U;

constexpr std::uint32_t continuation_mask = 0xC0U;
constexpr std::uint32_t continuation_pattern = 0x80U;
constexpr std::uint32_t continuation_payload_mask = 0x3FU;
constexpr std::uint32_t continuation_payload_bits = 6U;

// Unicode reserves the surrogate range for UTF-16 encoding. Those values are not Unicode scalar
// values and therefore cannot be represented by a well-formed UTF-8 sequence.
constexpr std::uint32_t first_surrogate_code_point = 0xD800U;
constexpr std::uint32_t last_surrogate_code_point = 0xDFFFU;
constexpr std::uint32_t maximum_code_point = 0x10FFFFU;

/// @brief Checks whether a decoded integer is a representable Unicode scalar value.
[[nodiscard]] constexpr bool is_scalar_value(const std::uint32_t code_point) noexcept
{
    return code_point <= maximum_code_point &&
           (code_point < first_surrogate_code_point || code_point > last_surrogate_code_point);
}

} // namespace

std::optional<DecodedScalar> decode_one(const std::string_view input) noexcept
{
    if (input.empty())
    {
        return std::nullopt;
    }
    const auto leading_byte = static_cast<unsigned char>(input.front());

    // ASCII occupies one byte and needs no continuation-byte decoding.
    if (leading_byte < ascii_limit)
    {
        return DecodedScalar{static_cast<char32_t>(leading_byte), 1};
    }

    // Select the sequence shape from the fixed prefix of its leading byte. `minimum` is the
    // smallest code point that requires this many bytes and later rejects overlong encodings.
    std::size_t length{};
    std::uint32_t code_point{};
    std::uint32_t minimum{};
    if ((leading_byte & two_byte_leading_mask) == two_byte_leading_pattern)
    {
        length = two_byte_sequence_length;
        code_point = leading_byte & two_byte_leading_payload_mask;
        minimum = two_byte_minimum_code_point;
    }
    else if ((leading_byte & three_byte_leading_mask) == three_byte_leading_pattern)
    {
        length = three_byte_sequence_length;
        code_point = leading_byte & three_byte_leading_payload_mask;
        minimum = three_byte_minimum_code_point;
    }
    else if ((leading_byte & four_byte_leading_mask) == four_byte_leading_pattern)
    {
        length = four_byte_sequence_length;
        code_point = leading_byte & four_byte_leading_payload_mask;
        minimum = four_byte_minimum_code_point;
    }
    else
    {
        return std::nullopt;
    }

    // A recognized leading byte must still have enough bytes remaining for the whole scalar.
    if (input.size() < length)
    {
        return std::nullopt;
    }

    // Each continuation byte has the form 10xxxxxx. Append its six payload bits to the value
    // already extracted from the leading byte.
    for (std::size_t continuation = 1; continuation < length; ++continuation)
    {
        const auto byte = static_cast<unsigned char>(input[continuation]);
        if ((byte & continuation_mask) != continuation_pattern)
        {
            return std::nullopt;
        }
        code_point = (code_point << continuation_payload_bits) | (byte & continuation_payload_mask);
    }

    // The lower bound excludes alternate, longer encodings of the same scalar. The scalar check
    // rejects UTF-16 surrogates and values beyond Unicode's U+10FFFF limit.
    if (code_point < minimum || !is_scalar_value(code_point))
    {
        return std::nullopt;
    }
    return DecodedScalar{static_cast<char32_t>(code_point), length};
}

bool is_valid(const std::string_view input) noexcept
{
    std::size_t index{};
    while (index < input.size())
    {
        const auto scalar = decode_one(input.substr(index));
        if (!scalar)
        {
            return false;
        }
        index += scalar->byte_count;
    }
    return true;
}

bool append_code_point(std::string &output, const char32_t code_point)
{
    const auto value = static_cast<std::uint32_t>(code_point);

    // Reject invalid input before appending anything so failure leaves `output` unchanged.
    if (!is_scalar_value(value))
    {
        return false;
    }

    // Choose the shortest legal sequence. A leading byte carries the highest payload bits; each
    // continuation byte carries the next six bits under its fixed 10xxxxxx prefix.
    if (value < ascii_limit)
    {
        output.push_back(static_cast<char>(value));
    }
    else if (value < three_byte_minimum_code_point)
    {
        output.push_back(
            static_cast<char>(two_byte_leading_pattern | (value >> continuation_payload_bits)));
        output.push_back(
            static_cast<char>(continuation_pattern | (value & continuation_payload_mask)));
    }
    else if (value < four_byte_minimum_code_point)
    {
        output.push_back(static_cast<char>(three_byte_leading_pattern |
                                           (value >> (continuation_payload_bits * 2U))));
        output.push_back(
            static_cast<char>(continuation_pattern |
                              ((value >> continuation_payload_bits) & continuation_payload_mask)));
        output.push_back(
            static_cast<char>(continuation_pattern | (value & continuation_payload_mask)));
    }
    else
    {
        output.push_back(static_cast<char>(four_byte_leading_pattern |
                                           (value >> (continuation_payload_bits * 3U))));
        output.push_back(static_cast<char>(
            continuation_pattern |
            ((value >> (continuation_payload_bits * 2U)) & continuation_payload_mask)));
        output.push_back(
            static_cast<char>(continuation_pattern |
                              ((value >> continuation_payload_bits) & continuation_payload_mask)));
        output.push_back(
            static_cast<char>(continuation_pattern | (value & continuation_payload_mask)));
    }
    return true;
}

} // namespace sparenode::text::utf8
