#include "windows_handle.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sparenode::filesystem::detail
{

void WindowsHandleCloser::operator()(void *handle) const noexcept
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    {
        static_cast<void>(CloseHandle(handle));
    }
}

std::error_code last_windows_error() noexcept
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

} // namespace sparenode::filesystem::detail

#endif
