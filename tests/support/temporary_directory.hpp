#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace sparenode::test
{

/// @brief Owns a unique temporary directory used by one filesystem test.
class TemporaryDirectory final
{
  public:
    /// @brief Creates a uniquely named directory below the system temporary directory.
    /// @param[in] name_prefix Human-readable prefix used for the directory name.
    explicit TemporaryDirectory(const std::string_view name_prefix)
    {
        static std::atomic_size_t sequence = 0;
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                (std::string(name_prefix) + '-' + std::to_string(timestamp) + '-' +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }

    /// @brief Removes the temporary directory and all test-created contents.
    ~TemporaryDirectory()
    {
        std::error_code ignored_error;
        std::filesystem::remove_all(path_, ignored_error);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    TemporaryDirectory(TemporaryDirectory &&) = delete;
    TemporaryDirectory &operator=(TemporaryDirectory &&) = delete;

    /// @brief Returns the temporary directory path.
    /// @return Stable path reference valid for this object's lifetime.
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_; ///< Directory removed when the fixture is destroyed.
};

} // namespace sparenode::test
