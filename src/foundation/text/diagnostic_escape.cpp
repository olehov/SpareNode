#include "sparenode/foundation/text/diagnostic_escape.hpp"

#include <ostream>

#include "sparenode/foundation/text/utf8.hpp"

namespace sparenode::text
{
namespace
{

constexpr std::string_view hexadecimal_digits = "0123456789ABCDEF";
constexpr char32_t first_c1_control = 0x80U;
constexpr char32_t last_c1_control = 0x9FU;

/// @brief Appends one byte in the stable `\\xNN` diagnostic form.
void append_hexadecimal_byte(std::ostream &output, const unsigned char byte)
{
    output << "\\x" << hexadecimal_digits[byte >> 4U] << hexadecimal_digits[byte & 0x0FU];
}

/// @brief Appends one decoded multi-byte scalar either verbatim or as escaped source bytes.
void append_scalar_bytes(std::ostream &output, const std::string_view bytes, const bool escape)
{
    for (const unsigned char byte : bytes)
    {
        if (escape)
        {
            append_hexadecimal_byte(output, byte);
        }
        else
        {
            output << static_cast<char>(byte);
        }
    }
}

} // namespace

void append_safe_diagnostic(std::ostream &output, const std::string_view value,
                            const DiagnosticEscapeStyle style)
{
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        const auto scalar = utf8::decode_one(value.substr(index));
        if (scalar && scalar->byte_count > 1)
        {
            const bool c1_control =
                scalar->value >= first_c1_control && scalar->value <= last_c1_control;
            append_scalar_bytes(output, value.substr(index, scalar->byte_count), c1_control);
            index += scalar->byte_count - 1;
            continue;
        }
        const auto byte = static_cast<unsigned char>(value[index]);
        if (style == DiagnosticEscapeStyle::log)
        {
            switch (byte)
            {
            case '\\':
                output << "\\\\";
                continue;
            case '\n':
                output << "\\n";
                continue;
            case '\r':
                output << "\\r";
                continue;
            case '\t':
                output << "\\t";
                continue;
            default:
                break;
            }
        }
        constexpr unsigned char first_visible_ascii_byte = 0x20U;
        constexpr unsigned char delete_control_byte = 0x7FU;
        const bool malformed_c1_byte = static_cast<char32_t>(byte) >= first_c1_control &&
                                       static_cast<char32_t>(byte) <= last_c1_control;
        if (byte < first_visible_ascii_byte || byte == delete_control_byte || malformed_c1_byte)
        {
            append_hexadecimal_byte(output, byte);
        }
        else
        {
            output << static_cast<char>(byte);
        }
    }
}

} // namespace sparenode::text
