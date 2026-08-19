#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sparenode::test
{

/// @brief Owns a unique temporary directory used by one filesystem test.
class TemporaryDirectory final
{
  public:
    /// @brief Creates a uniquely named directory below the system temporary directory.
    /// @param[in] name_prefix Human-readable prefix used for the directory name.
    /// @throws std::filesystem::filesystem_error When the temporary directory cannot be created.
    /// @throws std::runtime_error When unique-name attempts are exhausted.
    explicit TemporaryDirectory(const std::string_view name_prefix)
    {
        static std::atomic_size_t sequence = 0;
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto temporary_root = std::filesystem::temp_directory_path();
        constexpr std::size_t max_creation_attempts = 100;

        for (std::size_t attempt = 0; attempt < max_creation_attempts; ++attempt)
        {
            auto candidate =
                temporary_root / (std::string(name_prefix) + '-' + std::to_string(timestamp) + '-' +
                                  std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            if (std::filesystem::create_directory(candidate))
            {
                path_ = std::move(candidate);
                return;
            }
        }

        throw std::runtime_error("Cannot create a unique temporary test directory");
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
