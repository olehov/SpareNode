#include "windows_handle_path.hpp"
#include "windows_handle.hpp"

#ifdef _WIN32

#include <cstddef>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sparenode::filesystem::detail
{

bool windows_path_components_equal(const std::filesystem::path &left,
                                   const std::filesystem::path &right) noexcept
{
    const auto &left_text = left.native();
    const auto &right_text = right.native();
    return CompareStringOrdinal(left_text.c_str(), static_cast<int>(left_text.size()),
                                right_text.c_str(), static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
}

Result<std::filesystem::path, std::error_code> query_final_windows_path(void *handle)
{
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;

    // Most local paths fit without a second allocation; longer paths resize to the exact
    // length reported by GetFinalPathNameByHandleW below.
    constexpr std::size_t initial_capacity = 512;
    std::vector<wchar_t> buffer(initial_capacity);
    auto length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length == 0)
    {
        return unexpected(last_windows_error());
    }
    if (length >= buffer.size())
    {
        buffer.resize(static_cast<std::size_t>(length) + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           flags);
        if (length == 0 || length >= buffer.size())
        {
            return unexpected(last_windows_error());
        }
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).lexically_normal();
}

} // namespace sparenode::filesystem::detail

#endif
