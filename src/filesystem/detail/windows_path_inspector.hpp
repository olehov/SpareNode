#pragma once

#ifdef _WIN32

#include <cstdint>
#include <filesystem>

#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Identifies why a Windows filesystem object could not be inspected safely.
enum class WindowsPathInspectionError : std::uint8_t
{
    system_error,              ///< A required Win32 handle or metadata operation failed.
    unsupported_reparse_point, ///< The object uses a reparse tag outside the supported policy.
    ambiguous_component,       ///< Exact target spelling would change under legacy Win32 rules.
};

/// @brief Opens a Windows object and returns the fully resolved path named by its handle.
/// @param[in] path Existing filesystem object to inspect.
/// @return Normalized DOS or UNC path, or a fail-closed inspection error.
[[nodiscard]] Result<std::filesystem::path, WindowsPathInspectionError>
inspect_windows_path(const std::filesystem::path &path);

} // namespace sparenode::filesystem::detail

#endif
