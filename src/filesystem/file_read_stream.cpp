#include "sparenode/filesystem/file_read_stream.hpp"

namespace sparenode::filesystem
{

std::uint64_t FileReadStream::size() const noexcept
{
    return size_;
}

const char *to_string(const FileReadErrorCode code) noexcept
{
    switch (code)
    {
    case FileReadErrorCode::invalid_path:
        return "file path is invalid";
    case FileReadErrorCode::not_found:
        return "file does not exist";
    case FileReadErrorCode::not_regular_file:
        return "requested path is not a regular file";
    case FileReadErrorCode::permission_denied:
        return "file access was denied";
    case FileReadErrorCode::outside_shared_root:
        return "file is outside the shared root";
    case FileReadErrorCode::filesystem_failure:
        return "file read failed";
    case FileReadErrorCode::cancelled:
        return "file read was cancelled";
    case FileReadErrorCode::resource_allocation_failed:
        return "file stream memory allocation failed";
    }
    return "unknown file read error";
}

} // namespace sparenode::filesystem
