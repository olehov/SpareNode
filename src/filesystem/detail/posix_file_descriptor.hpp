#pragma once

#ifndef _WIN32

#include <cerrno>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Owns one POSIX file descriptor and closes it at scope exit.
class FileDescriptor final
{
  public:
    /// @brief Takes ownership of a descriptor returned by a POSIX open operation.
    /// @param[in] descriptor Valid descriptor whose ownership is transferred.
    explicit FileDescriptor(const int descriptor) noexcept : descriptor_(descriptor)
    {
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    /// @brief Transfers descriptor ownership.
    /// @param[in,out] other Owner whose descriptor is transferred.
    FileDescriptor(FileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }

    FileDescriptor &operator=(FileDescriptor &&) = delete;

    /// @brief Closes the descriptor when this object still owns one.
    ~FileDescriptor()
    {
        if (descriptor_ >= 0)
        {
            static_cast<void>(::close(descriptor_));
        }
    }

    /// @brief Returns the descriptor without releasing ownership.
    /// @return Owned descriptor or -1 after ownership transfer.
    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

  private:
    int descriptor_{-1}; ///< Owned descriptor, or -1 after ownership transfer.
};

/// @brief Captures the calling thread's current POSIX errno value.
/// @return Portable generic-category error code.
[[nodiscard]] inline std::error_code last_posix_error() noexcept
{
    return {errno, std::generic_category()};
}

/// @brief Resolves the stable filesystem object owned by a Linux descriptor.
/// @param[in] descriptor Open descriptor whose procfs link is inspected.
/// @return Absolute normalized path or a portable filesystem error.
[[nodiscard]] inline Result<std::filesystem::path, std::error_code>
descriptor_path(const FileDescriptor &descriptor)
{
    const auto proc_path =
        std::filesystem::path("/proc/self/fd") / std::to_string(descriptor.get());
    std::error_code error;
    auto path = std::filesystem::read_symlink(proc_path, error);
    if (error)
    {
        return unexpected(error);
    }
    if (!path.is_absolute())
    {
        return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    return path.lexically_normal();
}

} // namespace sparenode::filesystem::detail

#endif
