#include "windows_path_inspector.hpp"

#ifdef _WIN32

#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace sparenode::filesystem::detail
{
namespace
{

/// @brief Closes an owned Win32 handle.
struct HandleCloser
{
    /// @brief Releases a valid Win32 handle.
    /// @param[in] handle Handle returned by CreateFileW.
    void operator()(void *handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            static_cast<void>(CloseHandle(handle));
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;
using InspectionResult = Result<std::filesystem::path, WindowsPathInspectionError>;
using ReparseTargetResult =
    Result<std::optional<std::filesystem::path>, WindowsPathInspectionError>;

/// @brief Detects components that legacy Win32 access would silently reinterpret.
/// @param[in] path Native spelling to inspect without opening it.
/// @return `true` when a non-special component ends in an ASCII period or space.
[[nodiscard]] bool has_ambiguous_component(const std::filesystem::path &path) noexcept
{
    const auto &text = path.native();
    std::size_t start = 0;
    while (start < text.size())
    {
        const auto end = text.find_first_of(L"\\/", start);
        const auto component = std::wstring_view(text).substr(start, end - start);
        if (!component.empty() && component != L"." && component != L".." &&
            (component.back() == L'.' || component.back() == L' '))
        {
            return true;
        }
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    return false;
}

/// @brief Opens a filesystem object with optional final reparse traversal suppression.
/// @param[in] path Existing object to open.
/// @param[in] open_reparse_point Whether the handle names the final redirection object itself.
/// @return Owned handle, or a system inspection error.
[[nodiscard]] Result<UniqueHandle, WindowsPathInspectionError>
open_path(const std::filesystem::path &path, const bool open_reparse_point)
{
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    auto flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (open_reparse_point)
    {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    const auto handle =
        CreateFileW(path.c_str(), 0, sharing, nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    return UniqueHandle(handle);
}

/// @brief Reads an unsigned integer from an alignment-independent native buffer.
/// @tparam Value Unsigned Win32 integer type to copy.
/// @param[in] buffer Buffer containing reparse data.
/// @param[in] offset Byte offset of the integer.
/// @return Copied value, or an error when the field is outside returned data.
template <typename Value>
[[nodiscard]] Result<Value, WindowsPathInspectionError>
read_native_value(const std::span<const std::byte> buffer, const std::size_t offset)
{
    if (offset > buffer.size() || sizeof(Value) > buffer.size() - offset)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    Value value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

/// @brief Converts an NT substitute name into an exact extended-length Win32 path.
/// @param[in] target Substitute name stored in a junction or absolute symbolic link.
/// @return Extended DOS/UNC path, or an error for unsupported native namespaces.
[[nodiscard]] InspectionResult expand_native_target(const std::wstring_view target)
{
    constexpr std::wstring_view nt_unc_prefix = LR"(\??\UNC\)";
    constexpr std::wstring_view nt_prefix = LR"(\??\)";
    constexpr std::wstring_view extended_prefix = LR"(\\?\)";
    if (target.starts_with(nt_unc_prefix))
    {
        return std::filesystem::path(std::wstring(LR"(\\?\UNC\)") +
                                     std::wstring(target.substr(nt_unc_prefix.size())));
    }
    if (target.starts_with(nt_prefix))
    {
        return std::filesystem::path(std::wstring(extended_prefix) +
                                     std::wstring(target.substr(nt_prefix.size())));
    }
    if (target.starts_with(extended_prefix))
    {
        return std::filesystem::path(target);
    }
    return unexpected(WindowsPathInspectionError::system_error);
}

/// @brief Extracts and validates the substitute name from raw reparse data.
/// @param[in] buffer Complete bytes returned by FSCTL_GET_REPARSE_POINT.
/// @param[in] path_buffer_offset Tag-specific offset of the UTF-16 path buffer.
/// @return Stored substitute name without a terminator.
[[nodiscard]] Result<std::wstring, WindowsPathInspectionError>
read_substitute_name(const std::span<const std::byte> buffer, const std::size_t path_buffer_offset)
{
    constexpr std::size_t substitute_offset_field = 8;
    constexpr std::size_t substitute_length_field = 10;
    auto offset = read_native_value<WORD>(buffer, substitute_offset_field);
    auto length = read_native_value<WORD>(buffer, substitute_length_field);
    if (!offset || !length || length.value() % sizeof(wchar_t) != 0)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    const auto start = path_buffer_offset + offset.value();
    if (start > buffer.size() || length.value() > buffer.size() - start)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    std::wstring result(length.value() / sizeof(wchar_t), L'\0');
    std::memcpy(result.data(), buffer.data() + start, length.value());
    return result;
}

/// @brief Reads a supported symlink or junction target from an opened reparse point.
/// @param[in] handle Handle opened with FILE_FLAG_OPEN_REPARSE_POINT.
/// @param[in] link_path Path naming the reparse point itself.
/// @param[in] tag Reparse tag reported for the handle.
/// @return Target path to inspect as the next hop.
[[nodiscard]] InspectionResult
read_supported_target(const HANDLE handle, const std::filesystem::path &link_path, const DWORD tag)
{
    alignas(DWORD) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
    DWORD returned_bytes = 0;
    if (DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, storage.data(),
                        static_cast<DWORD>(storage.size()), &returned_bytes, nullptr) == FALSE)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    constexpr std::size_t reparse_header_size = 8;
    if (returned_bytes > storage.size())
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    auto buffer = std::span<const std::byte>(storage).first(returned_bytes);
    auto stored_tag = read_native_value<DWORD>(buffer, 0);
    auto data_length = read_native_value<WORD>(buffer, 4);
    if (buffer.size() < reparse_header_size || !stored_tag || !data_length ||
        stored_tag.value() != tag || data_length.value() > buffer.size() - reparse_header_size)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    buffer = buffer.first(reparse_header_size + data_length.value());
    constexpr std::size_t mount_path_offset = 16;
    constexpr std::size_t symlink_path_offset = 20;
    auto target = read_substitute_name(buffer, tag == IO_REPARSE_TAG_SYMLINK ? symlink_path_offset
                                                                             : mount_path_offset);
    if (!target)
    {
        return unexpected(target.error());
    }
    if (tag == IO_REPARSE_TAG_SYMLINK)
    {
        constexpr std::size_t symlink_flags_field = 16;
        constexpr DWORD symlink_flag_relative = 1;
        auto flags = read_native_value<DWORD>(buffer, symlink_flags_field);
        if (!flags)
        {
            return unexpected(flags.error());
        }
        if ((flags.value() & symlink_flag_relative) != 0)
        {
            return (link_path.parent_path() / target.value()).lexically_normal();
        }
    }
    return expand_native_target(target.value());
}

/// @brief Returns one supported redirection target without following the final object.
/// @param[in] path Existing path whose final component is inspected.
/// @return Empty optional for an ordinary object, or its supported target path.
[[nodiscard]] ReparseTargetResult read_reparse_target(const std::filesystem::path &path)
{
    auto handle = open_path(path, true);
    if (!handle)
    {
        return unexpected(handle.error());
    }
    FILE_ATTRIBUTE_TAG_INFO information{};
    if (GetFileInformationByHandleEx(handle->get(), FileAttributeTagInfo, &information,
                                     sizeof(information)) == FALSE)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    if ((information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
    {
        return std::optional<std::filesystem::path>{};
    }
    if (information.ReparseTag != IO_REPARSE_TAG_SYMLINK &&
        information.ReparseTag != IO_REPARSE_TAG_MOUNT_POINT)
    {
        return unexpected(WindowsPathInspectionError::unsupported_reparse_point);
    }
    auto target = read_supported_target(handle->get(), path, information.ReparseTag);
    if (!target)
    {
        return unexpected(target.error());
    }
    return std::optional<std::filesystem::path>(std::move(target).value());
}

/// @brief Reads the normalized final name represented by an open handle.
/// @param[in] handle Handle whose path is queried.
/// @return Absolute extended-length DOS or UNC path.
[[nodiscard]] InspectionResult query_final_path(const HANDLE handle)
{
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    constexpr std::size_t initial_capacity = 512;
    std::vector<wchar_t> buffer(initial_capacity);
    auto length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length == 0)
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }
    if (length >= buffer.size())
    {
        buffer.resize(static_cast<std::size_t>(length) + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           flags);
        if (length == 0 || length >= buffer.size())
        {
            return unexpected(WindowsPathInspectionError::system_error);
        }
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).lexically_normal();
}

/// @brief Appends unprocessed path components after a resolved redirection target.
/// @param[in] target Target of the current reparse point.
/// @param[in] current First unprocessed component from the original path.
/// @param[in] end End iterator of the original path.
/// @return Combined path to inspect from its root on the next pass.
[[nodiscard]] std::filesystem::path
append_remaining(std::filesystem::path target, std::filesystem::path::const_iterator current,
                 const std::filesystem::path::const_iterator end)
{
    for (; current != end; ++current)
    {
        target /= *current;
    }
    return target.lexically_normal();
}

} // namespace

Result<std::filesystem::path, WindowsPathInspectionError>
inspect_windows_path(const std::filesystem::path &path)
{
    std::error_code error;
    auto pending = std::filesystem::absolute(path, error).lexically_normal();
    if (error || !pending.has_root_path())
    {
        return unexpected(WindowsPathInspectionError::system_error);
    }

    constexpr std::size_t maximum_reparse_hops = 63;
    for (std::size_t hop = 0; hop <= maximum_reparse_hops; ++hop)
    {
        if (has_ambiguous_component(pending))
        {
            return unexpected(WindowsPathInspectionError::ambiguous_component);
        }
        auto prefix = pending.root_path();
        bool redirected = false;
        const auto relative = pending.relative_path();
        for (auto component = relative.begin(); component != relative.end(); ++component)
        {
            prefix /= *component;
            auto target = read_reparse_target(prefix);
            if (!target)
            {
                return unexpected(target.error());
            }
            if (target->has_value())
            {
                pending = append_remaining(std::move(target->value()), std::next(component),
                                           relative.end());
                redirected = true;
                break;
            }
        }
        if (!redirected)
        {
            auto handle = open_path(pending, false);
            return handle ? query_final_path(handle->get())
                          : InspectionResult(unexpected(handle.error()));
        }
    }
    return unexpected(WindowsPathInspectionError::system_error);
}

} // namespace sparenode::filesystem::detail

#endif
