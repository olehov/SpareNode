#include "sparenode/filesystem/safe_path.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace sparenode::filesystem
{
namespace
{

/// @brief Checks whether a byte sequence is a well-formed UTF-8 string.
/// @param[in] input Bytes to validate without modifying them.
/// @return `true` when every byte belongs to one valid Unicode scalar sequence.
[[nodiscard]] bool is_valid_utf8(const std::string_view input) noexcept
{
    std::size_t index = 0;
    while (index < input.size())
    {
        const auto leading_byte = static_cast<std::uint8_t>(input[index]);
        if (leading_byte <= 0x7F)
        {
            ++index;
            continue;
        }

        std::size_t sequence_length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum_code_point = 0;
        if ((leading_byte & 0xE0) == 0xC0)
        {
            sequence_length = 2;
            code_point = leading_byte & 0x1F;
            minimum_code_point = 0x80;
        }
        else if ((leading_byte & 0xF0) == 0xE0)
        {
            sequence_length = 3;
            code_point = leading_byte & 0x0F;
            minimum_code_point = 0x800;
        }
        else if ((leading_byte & 0xF8) == 0xF0)
        {
            sequence_length = 4;
            code_point = leading_byte & 0x07;
            minimum_code_point = 0x10000;
        }
        else
        {
            return false;
        }

        if (sequence_length > input.size() - index)
        {
            return false;
        }

        for (std::size_t offset = 1; offset < sequence_length; ++offset)
        {
            const auto continuation_byte = static_cast<std::uint8_t>(input[index + offset]);
            if ((continuation_byte & 0xC0) != 0x80)
            {
                return false;
            }
            code_point = (code_point << 6) | (continuation_byte & 0x3F);
        }

        constexpr std::uint32_t first_surrogate = 0xD800;
        constexpr std::uint32_t last_surrogate = 0xDFFF;
        constexpr std::uint32_t maximum_code_point = 0x10FFFF;
        if (code_point < minimum_code_point ||
            (code_point >= first_surrogate && code_point <= last_surrogate) ||
            code_point > maximum_code_point)
        {
            return false;
        }

        index += sequence_length;
    }

    return true;
}

/// @brief Checks path containment using complete native path components.
/// @param[in] candidate Absolute path whose prefix is inspected.
/// @param[in] shared_root Validated shared root required as the complete prefix.
/// @return `true` when candidate is the root or one of its descendants.
[[nodiscard]] bool is_within_root(const std::filesystem::path &candidate,
                                  const configuration::SharedRoot &shared_root) noexcept
{
    const auto &root = shared_root.path();
    auto candidate_component = candidate.begin();
    for (const auto &root_component : root)
    {
        if (candidate_component == candidate.end() || *candidate_component != root_component)
        {
            return false;
        }
        ++candidate_component;
    }

    return true;
}

} // namespace

SafePath::SafePath(std::filesystem::path resolved_path) : path_(std::move(resolved_path))
{
}

Result<SafePath, SafePathError> SafePath::resolve(const configuration::SharedRoot &shared_root,
                                                  const std::string_view requested_path)
{
    if (!is_valid_utf8(requested_path))
    {
        return unexpected(
            SafePathError{SafePathErrorCode::invalid_encoding, std::string(requested_path)});
    }
    if (requested_path.find('\0') != std::string_view::npos)
    {
        return unexpected(
            SafePathError{SafePathErrorCode::embedded_null, std::string(requested_path)});
    }

    const std::u8string requested_path_utf8(requested_path.begin(), requested_path.end());
    const std::filesystem::path relative_path(requested_path_utf8);
    if (relative_path.has_root_name() || relative_path.has_root_directory())
    {
        return unexpected(
            SafePathError{SafePathErrorCode::rooted_path, std::string(requested_path)});
    }

    const auto normalized_relative_path = relative_path.lexically_normal();
    auto resolved_path = normalized_relative_path.empty() || normalized_relative_path == "."
                             ? shared_root.path()
                             : (shared_root.path() / normalized_relative_path).lexically_normal();
    if (!is_within_root(resolved_path, shared_root))
    {
        return unexpected(
            SafePathError{SafePathErrorCode::outside_shared_root, std::string(requested_path)});
    }

    return SafePath(std::move(resolved_path));
}

const std::filesystem::path &SafePath::path() const noexcept
{
    return path_;
}

const char *to_string(const SafePathErrorCode code) noexcept
{
    switch (code)
    {
    case SafePathErrorCode::invalid_encoding:
        return "the requested path is not valid UTF-8";
    case SafePathErrorCode::embedded_null:
        return "the requested path contains a null byte";
    case SafePathErrorCode::rooted_path:
        return "the requested path is not relative";
    case SafePathErrorCode::outside_shared_root:
        return "the requested path escapes the shared root";
    }

    return "unknown safe-path error";
}

} // namespace sparenode::filesystem
