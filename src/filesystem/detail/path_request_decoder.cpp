#include "sparenode/filesystem/detail/path_request_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sparenode::filesystem::detail
{

namespace
{

/// @brief Converts one ASCII hexadecimal digit to its numeric value.
/// @param[in] character Byte to classify without locale-sensitive rules.
/// @return Value from zero to fifteen, or no value for a non-hexadecimal byte.
[[nodiscard]] std::optional<std::uint8_t> hex_value(const char character) noexcept
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

/// @brief Detects a valid percent escape left after the first decoding pass.
/// @param[in] decoded_path Decoded bytes that must not be decoded a second time.
/// @return `true` when another decoder could reinterpret at least one byte.
[[nodiscard]] bool contains_percent_escape(const std::string_view decoded_path) noexcept
{
    for (std::size_t index = 0; index + 2 < decoded_path.size(); ++index)
    {
        if (decoded_path[index] == '%' && hex_value(decoded_path[index + 1]).has_value() &&
            hex_value(decoded_path[index + 2]).has_value())
        {
            return true;
        }
    }
    return false;
}

} // namespace

Result<std::string, PathRequestDecodeError>
decode_path_request(const std::string_view requested_path)
{
    std::string decoded_path;
    decoded_path.reserve(requested_path.size());

    for (std::size_t index = 0; index < requested_path.size();)
    {
        if (requested_path[index] != '%')
        {
            decoded_path.push_back(requested_path[index]);
            ++index;
            continue;
        }

        if (requested_path.size() - index < 3)
        {
            return unexpected(PathRequestDecodeError::invalid_percent_encoding);
        }
        const auto high = hex_value(requested_path[index + 1]);
        const auto low = hex_value(requested_path[index + 2]);
        if (!high.has_value() || !low.has_value())
        {
            return unexpected(PathRequestDecodeError::invalid_percent_encoding);
        }

        decoded_path.push_back(static_cast<char>((high.value() << 4U) | low.value()));
        index += 3;
    }

    if (contains_percent_escape(decoded_path))
    {
        return unexpected(PathRequestDecodeError::invalid_percent_encoding);
    }

    std::ranges::replace(decoded_path, '\\', '/');
    return decoded_path;
}

} // namespace sparenode::filesystem::detail
