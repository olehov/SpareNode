#include "sparenode/filesystem/safe_path.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "sparenode/filesystem/detail/path_request_decoder.hpp"

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

/// @brief Detects absolute and drive-qualified syntax consistently on every host.
/// @param[in] normalized_request Decoded path whose separators are all `/`.
/// @return `true` for slash-rooted, UNC-style, or drive-qualified input.
[[nodiscard]] bool has_portable_root(const std::string_view normalized_request) noexcept
{
    if (normalized_request.empty())
    {
        return false;
    }
    if (normalized_request.front() == '/')
    {
        return true;
    }

    const char first_character = normalized_request.front();
    const bool has_drive_letter = (first_character >= 'A' && first_character <= 'Z') ||
                                  (first_character >= 'a' && first_character <= 'z');
    return normalized_request.size() >= 2 && has_drive_letter && normalized_request[1] == ':';
}

#ifdef _WIN32
/// @brief Compares a Windows component name with an uppercase ASCII name.
/// @tparam Size Number of code units in the expected literal, including its null terminator.
/// @param[in] candidate Native component text whose ASCII letters are compared without case.
/// @param[in] expected Uppercase ASCII name to match.
/// @return `true` when both names are equal under ASCII case folding.
template <std::size_t Size>
[[nodiscard]] bool equals_ascii_case_insensitive(const std::wstring_view candidate,
                                                 const wchar_t (&expected)[Size]) noexcept
{
    if (candidate.size() != Size - 1)
    {
        return false;
    }

    for (std::size_t index = 0; index < candidate.size(); ++index)
    {
        const auto character = candidate[index];
        const auto uppercase_character = character >= L'a' && character <= L'z'
                                             ? static_cast<wchar_t>(character - L'a' + L'A')
                                             : character;
        if (uppercase_character != expected[index])
        {
            return false;
        }
    }

    return true;
}

/// @brief Checks whether a component base name aliases a reserved Win32 device.
/// @param[in] component Native component including any extension or stream suffix.
/// @return `true` for documented device aliases, matched case-insensitively.
[[nodiscard]] bool is_windows_device_name(const std::wstring_view component) noexcept
{
    const auto base_name = component.substr(0, component.find_first_of(L".:"));
    if (equals_ascii_case_insensitive(base_name, L"CON") ||
        equals_ascii_case_insensitive(base_name, L"PRN") ||
        equals_ascii_case_insensitive(base_name, L"AUX") ||
        equals_ascii_case_insensitive(base_name, L"NUL"))
    {
        return true;
    }

    if (base_name.size() != 4)
    {
        return false;
    }

    const auto prefix = base_name.substr(0, 3);
    const auto suffix = base_name.back();
    const bool has_reserved_prefix = equals_ascii_case_insensitive(prefix, L"COM") ||
                                     equals_ascii_case_insensitive(prefix, L"LPT");
    const bool has_reserved_suffix = (suffix >= L'1' && suffix <= L'9') || suffix == L'\u00B9' ||
                                     suffix == L'\u00B2' || suffix == L'\u00B3';
    return has_reserved_prefix && has_reserved_suffix;
}

/// @brief Checks whether a code unit is forbidden in an ordinary Win32 path component.
/// @param[in] character Native code unit to classify.
/// @return `true` for Win32-reserved punctuation or control characters.
[[nodiscard]] bool is_windows_forbidden_component_character(const wchar_t character) noexcept
{
    if (character >= 1 && character <= 31)
    {
        return true;
    }

    switch (character)
    {
    case L'<':
    case L'>':
    case L':':
    case L'"':
    case L'|':
    case L'?':
    case L'*':
        return true;
    default:
        return false;
    }
}

/// @brief Checks whether a relative component violates ordinary Win32 naming rules.
/// @param[in] path Relative native path whose components are inspected.
/// @return `true` for forbidden characters, aliases, or reserved device names.
[[nodiscard]] bool has_windows_invalid_component(const std::filesystem::path &path) noexcept
{
    for (const auto &component : path)
    {
        const auto &native_component = component.native();
        if (native_component == L"." || native_component == L".." || native_component.empty())
        {
            continue;
        }

        const auto final_character = native_component.back();
        const bool has_forbidden_character =
            std::ranges::any_of(native_component, is_windows_forbidden_component_character);
        if (has_forbidden_character || final_character == L' ' || final_character == L'.' ||
            is_windows_device_name(native_component))
        {
            return true;
        }
    }

    return false;
}
#endif

} // namespace

SafePath::SafePath(std::filesystem::path resolved_path) : path_(std::move(resolved_path))
{
}

Result<SafePath, SafePathError> SafePath::resolve(const configuration::SharedRoot &shared_root,
                                                  const std::string_view requested_path)
{
    if (requested_path.size() > maximum_requested_path_bytes)
    {
        return unexpected(SafePathError{SafePathErrorCode::path_too_long, {}});
    }

    auto decoded_request = detail::decode_path_request(requested_path);
    if (!decoded_request)
    {
        return unexpected(SafePathError{SafePathErrorCode::invalid_percent_encoding,
                                        std::string(requested_path)});
    }
    const auto &normalized_request = decoded_request.value();
    if (!is_valid_utf8(normalized_request))
    {
        return unexpected(
            SafePathError{SafePathErrorCode::invalid_encoding, std::string(requested_path)});
    }
    if (normalized_request.find('\0') != std::string_view::npos)
    {
        return unexpected(
            SafePathError{SafePathErrorCode::embedded_null, std::string(requested_path)});
    }

    if (has_portable_root(normalized_request))
    {
        return unexpected(
            SafePathError{SafePathErrorCode::rooted_path, std::string(requested_path)});
    }

    const std::u8string requested_path_utf8(normalized_request.begin(), normalized_request.end());
    const std::filesystem::path relative_path(requested_path_utf8);
    if (relative_path.has_root_name() || relative_path.has_root_directory())
    {
        return unexpected(
            SafePathError{SafePathErrorCode::rooted_path, std::string(requested_path)});
    }
#ifdef _WIN32
    if (has_windows_invalid_component(relative_path))
    {
        return unexpected(
            SafePathError{SafePathErrorCode::invalid_component, std::string(requested_path)});
    }
#endif

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
    case SafePathErrorCode::path_too_long:
        return "the requested path exceeds the application input limit";
    case SafePathErrorCode::invalid_percent_encoding:
        return "the requested path contains invalid or nested percent encoding";
    case SafePathErrorCode::invalid_encoding:
        return "the requested path is not valid UTF-8";
    case SafePathErrorCode::embedded_null:
        return "the requested path contains a null byte";
    case SafePathErrorCode::rooted_path:
        return "the requested path is not relative";
    case SafePathErrorCode::invalid_component:
        return "the requested path contains a platform-invalid component";
    case SafePathErrorCode::outside_shared_root:
        return "the requested path escapes the shared root";
    }

    return "unknown safe-path error";
}

} // namespace sparenode::filesystem
