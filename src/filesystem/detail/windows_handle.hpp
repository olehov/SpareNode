#pragma once

#ifdef _WIN32

#include <memory>
#include <system_error>

namespace sparenode::filesystem::detail
{

/// @brief Closes a valid Win32 filesystem handle.
struct WindowsHandleCloser
{
    /// @brief Releases a handle returned by a Win32 open operation.
    /// @param[in] handle Owned handle or an invalid sentinel.
    void operator()(void *handle) const noexcept;
};

/// @brief Provides unique RAII ownership for a Win32 filesystem handle.
using UniqueWindowsHandle = std::unique_ptr<void, WindowsHandleCloser>;

/// @brief Captures the calling thread's current Win32 error value.
/// @return Portable system-category error code.
[[nodiscard]] std::error_code last_windows_error() noexcept;

} // namespace sparenode::filesystem::detail

#endif
