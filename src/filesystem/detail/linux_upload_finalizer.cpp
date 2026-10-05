#include "sparenode/filesystem/detail/directory_entry_reader.hpp"

#ifndef _WIN32

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <linux/fs.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "posix_file_descriptor.hpp"
#include "temporary_name.hpp"

namespace sparenode::filesystem::detail
{
namespace
{

/// One bounded copy buffer; neither source size nor filename controls memory use.
constexpr std::size_t copy_buffer_bytes = std::size_t{64} * 1024;
constexpr std::size_t maximum_stage_attempts = 100;

/// @brief Removes an unpublished sibling even when copying or renaming fails.
struct StageCleanup
{
    int parent{};     ///< Stable destination-directory descriptor.
    std::string name; ///< Exclusive sibling name to remove.
    bool published{}; ///< A successful rename consumed the sibling name.

    ~StageCleanup()
    {
        if (!published)
        {
            static_cast<void>(::unlinkat(parent, name.c_str(), 0));
        }
    }
};

/// @brief Converts one native failure without losing its errno value.
[[nodiscard]] UploadFinalizationError failure(const UploadFinalizationErrorCode code) noexcept
{
    return {code, {}, last_posix_error()};
}

/// @brief Selects cancellation before expiry at every bounded I/O boundary.
[[nodiscard]] std::optional<UploadFinalizationError>
interruption(const TemporaryFileIoOptions &options) noexcept
{
    if (options.stop_token.stop_requested())
    {
        return UploadFinalizationError{UploadFinalizationErrorCode::cancelled};
    }
    if (options.deadline && std::chrono::steady_clock::now() >= options.deadline.value())
    {
        return UploadFinalizationError{UploadFinalizationErrorCode::deadline_exceeded};
    }
    return std::nullopt;
}

/// @brief Opens an exclusive private sibling relative to the verified parent descriptor.
[[nodiscard]] Result<std::pair<FileDescriptor, std::string>, UploadFinalizationError>
create_stage(const int parent)
{
    for (std::size_t attempt = 0; attempt < maximum_stage_attempts; ++attempt)
    {
        auto name = ".sparenode-upload-" + temporary_name_suffix() + ".tmp";
        const auto descriptor = ::openat(
            parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0)
        {
            return std::pair{FileDescriptor(descriptor), std::move(name)};
        }
        if (errno != EEXIST)
        {
            return unexpected(failure(UploadFinalizationErrorCode::staging_failed));
        }
    }
    return unexpected(UploadFinalizationError{UploadFinalizationErrorCode::staging_failed,
                                              {},
                                              std::make_error_code(std::errc::file_exists)});
}

/// @brief Copies exactly the completed source size to the unpublished sibling.
[[nodiscard]] Result<void, UploadFinalizationError>
copy_source(const FileDescriptor &source, const FileDescriptor &stage,
            const TemporaryFile &artifact, const TemporaryFileIoOptions &options)
{
    std::array<std::byte, copy_buffer_bytes> buffer{};
    std::uint64_t copied = 0;
    const auto expected_bytes = artifact.size();
    while (copied < expected_bytes)
    {
        if (const auto stopped = interruption(options); stopped)
        {
            return unexpected(stopped.value());
        }
        const auto requested = static_cast<std::size_t>(
            (std::min)(expected_bytes - copied, static_cast<std::uint64_t>(buffer.size())));
        const auto received = ::read(source.get(), buffer.data(), requested);
        if (received < 0 && errno == EINTR)
        {
            continue;
        }
        if (received == 0)
        {
            return unexpected(
                UploadFinalizationError{UploadFinalizationErrorCode::source_read_failed});
        }
        if (received < 0)
        {
            return unexpected(failure(UploadFinalizationErrorCode::source_read_failed));
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(received))
        {
            if (const auto stopped = interruption(options); stopped)
            {
                return unexpected(stopped.value());
            }
            const auto written = ::write(stage.get(), buffer.data() + offset,
                                         static_cast<std::size_t>(received) - offset);
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            if (written == 0)
            {
                return unexpected(
                    UploadFinalizationError{UploadFinalizationErrorCode::staging_write_failed});
            }
            if (written < 0)
            {
                return unexpected(failure(UploadFinalizationErrorCode::staging_write_failed));
            }
            offset += static_cast<std::size_t>(written);
        }
        copied += static_cast<std::uint64_t>(received);
    }
    return {};
}

/// @brief Atomically claims the destination without replacing an existing entry.
[[nodiscard]] Result<void, UploadFinalizationError>
publish_stage(StageCleanup &cleanup, const std::filesystem::path &native_name)
{
    const auto &destination = native_name.native();
    const auto renamed = ::syscall(SYS_renameat2, cleanup.parent, cleanup.name.c_str(),
                                   cleanup.parent, destination.c_str(), RENAME_NOREPLACE);
    if (renamed == 0)
    {
        cleanup.published = true;
        return {};
    }
    if (errno == EEXIST)
    {
        return unexpected(failure(UploadFinalizationErrorCode::destination_exists));
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP)
    {
        return unexpected(failure(UploadFinalizationErrorCode::publish_failed));
    }
    if (::linkat(cleanup.parent, cleanup.name.c_str(), cleanup.parent, destination.c_str(), 0) != 0)
    {
        return unexpected(failure(errno == EEXIST ? UploadFinalizationErrorCode::destination_exists
                                                  : UploadFinalizationErrorCode::publish_failed));
    }
    return {};
}

} // namespace

/// @brief Stages and publishes through one stable parent descriptor without replacement.
Result<void, UploadFinalizationError>
publish_confined_upload(const ConfinedDirectory &directory, const TemporaryFile &source,
                        const std::filesystem::path &native_name,
                        const TemporaryFileIoOptions &options)
{
    const FileDescriptor source_file(
        ::open(source.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (source_file.get() < 0)
    {
        return unexpected(failure(UploadFinalizationErrorCode::source_read_failed));
    }
    struct stat source_stat
    {
    };
    if (::fstat(source_file.get(), &source_stat) != 0 || !S_ISREG(source_stat.st_mode) ||
        source_stat.st_size < 0 || static_cast<std::uint64_t>(source_stat.st_size) != source.size())
    {
        return unexpected(failure(UploadFinalizationErrorCode::source_read_failed));
    }

    auto created = create_stage(directory.native_handle());
    if (!created)
    {
        return unexpected(UploadFinalizationError{
            UploadFinalizationErrorCode::staging_failed, {}, created.error().system_error});
    }
    auto [stage_file, stage_name] = std::move(created).value();
    StageCleanup cleanup{directory.native_handle(), std::move(stage_name)};
    if (auto copied = copy_source(source_file, stage_file, source, options); !copied)
    {
        return copied;
    }
    if (const auto stopped = interruption(options); stopped)
    {
        return unexpected(stopped.value());
    }
    if (::fsync(stage_file.get()) != 0)
    {
        return unexpected(failure(UploadFinalizationErrorCode::staging_flush_failed));
    }
    if (const auto stopped = interruption(options); stopped)
    {
        return unexpected(stopped.value());
    }

    return publish_stage(cleanup, native_name);
}

} // namespace sparenode::filesystem::detail

#endif
