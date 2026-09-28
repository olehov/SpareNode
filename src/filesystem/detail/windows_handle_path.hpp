#pragma once

#ifdef _WIN32

#include <filesystem>
#include <system_error>

#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Reads the normalized final DOS or UNC path owned by a Win32 handle.
/// @param[in] handle Valid filesystem handle returned by a Win32 open operation.
/// @return Absolute normalized path or the originating Win32 system error.
[[nodiscard]] Result<std::filesystem::path, std::error_code> query_final_windows_path(void *handle);

} // namespace sparenode::filesystem::detail

#endif
