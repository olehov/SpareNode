#include "sparenode/filesystem/temporary_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sparenode::filesystem
{
namespace
{

constexpr std::size_t maximum_creation_attempts = 100;
constexpr unsigned bits_per_hexadecimal_digit = 4;
constexpr unsigned uint64_bit_count = 64;
constexpr unsigned random_half_bit_count = 32;
constexpr std::size_t suffix_hexadecimal_digits = 32;
constexpr std::uint64_t sequence_mixing_constant = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t low_hexadecimal_digit_mask = 0x0FULL;

/// @brief Produces a process-unique candidate suffix without trusting it for exclusivity.
[[nodiscard]] std::string candidate_suffix()
{
    static std::atomic_uint64_t sequence{};
    const auto clock =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto count = sequence.fetch_add(1, std::memory_order_relaxed);
    std::random_device random;
    const auto entropy = (static_cast<std::uint64_t>(random()) << random_half_bit_count) ^ random();
    constexpr std::array hexadecimal{'0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result(suffix_hexadecimal_digits, '0');
    auto value = clock ^ (count * sequence_mixing_constant) ^ entropy;
    for (auto &digit : result)
    {
        digit = hexadecimal[value & low_hexadecimal_digit_mask];
        value = (value >> bits_per_hexadecimal_digit) ^
                (value << (uint64_bit_count - bits_per_hexadecimal_digit)) ^ count;
    }
    return result;
}

#ifdef _WIN32
[[nodiscard]] HANDLE native_handle(const std::intptr_t value) noexcept
{
    return reinterpret_cast<HANDLE>(value);
}
#endif

} // namespace

TemporaryFile::TemporaryFile(std::filesystem::path path, const std::intptr_t native_handle) noexcept
    : path_(std::move(path)), native_handle_(native_handle)
{
}

TemporaryFile::TemporaryFile(TemporaryFile &&other) noexcept
    : path_(std::move(other.path_)), native_handle_(std::exchange(other.native_handle_, -1)),
      size_(std::exchange(other.size_, 0)), completed_(std::exchange(other.completed_, false))
{
}

TemporaryFile &TemporaryFile::operator=(TemporaryFile &&other) noexcept
{
    if (this != &other)
    {
        cleanup();
        path_ = std::move(other.path_);
        native_handle_ = std::exchange(other.native_handle_, -1);
        size_ = std::exchange(other.size_, 0);
        completed_ = std::exchange(other.completed_, false);
    }
    return *this;
}

TemporaryFile::~TemporaryFile()
{
    cleanup();
}

Result<TemporaryFile, TemporaryFileError> TemporaryFile::create()
{
    try
    {
        std::error_code directory_error;
        const auto directory = std::filesystem::temp_directory_path(directory_error);
        if (directory_error)
        {
            return unexpected(TemporaryFileError{
                TemporaryFileErrorCode::temporary_directory_unavailable, directory_error.value()});
        }
        int last_error = 0;
        for (std::size_t attempt = 0; attempt < maximum_creation_attempts; ++attempt)
        {
            auto path = directory / (".sparenode-body-" + candidate_suffix() + ".tmp");
#ifdef _WIN32
            const auto handle =
                CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (handle != INVALID_HANDLE_VALUE)
            {
                return TemporaryFile(std::move(path), reinterpret_cast<std::intptr_t>(handle));
            }
            last_error = static_cast<int>(GetLastError());
            if (last_error != ERROR_FILE_EXISTS && last_error != ERROR_ALREADY_EXISTS)
            {
                break;
            }
#else
            const auto handle = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (handle >= 0)
            {
                return TemporaryFile(std::move(path), handle);
            }
            last_error = errno;
            if (last_error != EEXIST)
            {
                break;
            }
#endif
        }
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::create_failed, last_error});
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(
            TemporaryFileError{TemporaryFileErrorCode::resource_allocation_failed, 0});
    }
    catch (const std::system_error &error)
    {
        return unexpected(
            TemporaryFileError{TemporaryFileErrorCode::create_failed, error.code().value()});
    }
}

Result<void, TemporaryFileError> TemporaryFile::write(const std::span<const std::byte> bytes)
{
    if (native_handle_ == -1 || completed_)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::invalid_state, 0});
    }
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
#ifdef _WIN32
        const auto remaining = (std::min)(
            bytes.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
        DWORD written = 0;
        if (WriteFile(native_handle(native_handle_), bytes.data() + offset,
                      static_cast<DWORD>(remaining), &written, nullptr) == FALSE ||
            written == 0)
        {
            return unexpected(TemporaryFileError{TemporaryFileErrorCode::write_failed,
                                                 static_cast<int>(GetLastError())});
        }
        offset += written;
#else
        const auto written =
            ::write(static_cast<int>(native_handle_), bytes.data() + offset, bytes.size() - offset);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return unexpected(TemporaryFileError{TemporaryFileErrorCode::write_failed, errno});
        }
        if (written == 0)
        {
            return unexpected(TemporaryFileError{TemporaryFileErrorCode::write_failed, 0});
        }
        offset += static_cast<std::size_t>(written);
#endif
    }
    size_ += bytes.size();
    return {};
}

Result<void, TemporaryFileError> TemporaryFile::complete()
{
    if (native_handle_ == -1 || completed_)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::invalid_state, 0});
    }
#ifdef _WIN32
    if (FlushFileBuffers(native_handle(native_handle_)) == FALSE)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::flush_failed,
                                             static_cast<int>(GetLastError())});
    }
    const auto handle = native_handle(native_handle_);
    native_handle_ = -1;
    if (CloseHandle(handle) == FALSE)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::close_failed,
                                             static_cast<int>(GetLastError())});
    }
#else
    if (::fsync(static_cast<int>(native_handle_)) != 0)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::flush_failed, errno});
    }
    const auto handle = static_cast<int>(native_handle_);
    native_handle_ = -1;
    if (::close(handle) != 0)
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::close_failed, errno});
    }
#endif
    completed_ = true;
    return {};
}

Result<std::filesystem::path, TemporaryFileError> TemporaryFile::release()
{
    if (!completed_ || path_.empty())
    {
        return unexpected(TemporaryFileError{TemporaryFileErrorCode::invalid_state, 0});
    }
    return std::exchange(path_, {});
}

void TemporaryFile::cleanup() noexcept
{
    if (native_handle_ != -1)
    {
#ifdef _WIN32
        static_cast<void>(CloseHandle(native_handle(native_handle_)));
#else
        static_cast<void>(::close(static_cast<int>(native_handle_)));
#endif
        native_handle_ = -1;
    }
    if (!path_.empty())
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        path_.clear();
    }
}

const char *to_string(const TemporaryFileErrorCode code) noexcept
{
    switch (code)
    {
    case TemporaryFileErrorCode::temporary_directory_unavailable:
        return "temporary directory is unavailable";
    case TemporaryFileErrorCode::create_failed:
        return "temporary file creation failed";
    case TemporaryFileErrorCode::write_failed:
        return "temporary file write failed";
    case TemporaryFileErrorCode::flush_failed:
        return "temporary file flush failed";
    case TemporaryFileErrorCode::close_failed:
        return "temporary file close failed";
    case TemporaryFileErrorCode::invalid_state:
        return "temporary file state is invalid";
    case TemporaryFileErrorCode::resource_allocation_failed:
        return "temporary file memory allocation failed";
    }
    return "unknown temporary file error";
}

} // namespace sparenode::filesystem
