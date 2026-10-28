#include "sparenode/foundation/text/diagnostic_escape.hpp"

#include <ostream>

namespace sparenode::text
{
namespace
{

constexpr std::string_view hexadecimal_digits = "0123456789ABCDEF";

/// @brief Appends one byte in the stable `\\xNN` diagnostic form.
void append_hexadecimal_byte(std::ostream &output, const unsigned char byte)
{
    output << "\\x" << hexadecimal_digits[byte >> 4U] << hexadecimal_digits[byte & 0x0FU];
}

} // namespace

void append_safe_diagnostic(std::ostream &output, const std::string_view value,
                            const DiagnosticEscapeStyle style)
{
    for (const unsigned char byte : value)
    {
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
        if (byte < first_visible_ascii_byte || byte == delete_control_byte)
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
