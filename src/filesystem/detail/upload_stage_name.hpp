#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace sparenode::filesystem::detail
{

/// @brief Reserves names of unpublished uploads from public path resolution.
inline constexpr std::string_view upload_stage_prefix = ".sparenode-upload-";

/// @brief Matches a native path component against the reserved staging prefix.
[[nodiscard]] inline bool is_upload_stage_component(const std::filesystem::path &component) noexcept
{
    const auto &name = component.native();
#ifdef _WIN32
    if (name.size() < upload_stage_prefix.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < upload_stage_prefix.size(); ++index)
    {
        const auto character = name[index];
        const auto folded = character >= L'A' && character <= L'Z'
                                ? static_cast<wchar_t>(character - L'A' + L'a')
                                : character;
        if (folded != static_cast<wchar_t>(upload_stage_prefix[index]))
        {
            return false;
        }
    }
    return true;
#else
    return name.starts_with(upload_stage_prefix);
#endif
}

} // namespace sparenode::filesystem::detail
