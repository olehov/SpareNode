#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Identifies an entry using metadata read from a stable confined handle.
enum class ConfinedEntryKind : std::uint8_t
{
    regular_file,  ///< A regular file contained by the configured root.
    directory,     ///< A directory contained by the configured root.
    symbolic_link, ///< A link whose opened target remains inside the configured root.
    other          ///< Another filesystem object that can be inspected safely.
};

/// @brief Owns metadata obtained from one stable confined filesystem handle.
struct ConfinedEntryMetadata
{
    ConfinedEntryKind kind{};                     ///< Portable entry category.
    std::optional<std::uintmax_t> size;           ///< Regular-file bytes, otherwise empty.
    std::chrono::sys_seconds modification_time{}; ///< Last modification time in UTC seconds.
};

/// @brief Owns one verified directory context inside the configured shared root.
class ConfinedDirectory final
{
  public:
    ConfinedDirectory(const ConfinedDirectory &) = delete;
    ConfinedDirectory &operator=(const ConfinedDirectory &) = delete;

    /// @brief Transfers ownership of a verified platform directory context.
    ConfinedDirectory(ConfinedDirectory &&) noexcept;

    /// @brief Replaces this context with another verified platform directory context.
    /// @return This context after taking ownership.
    ConfinedDirectory &operator=(ConfinedDirectory &&) noexcept;

    /// @brief Releases the retained platform directory handle.
    ~ConfinedDirectory();

  private:
    struct Implementation;

    /// @brief Takes ownership of a platform-specific verified directory implementation.
    /// @param[in] implementation Stable directory handles and boundary state.
    explicit ConfinedDirectory(std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_; ///< Platform-specific stable handles.

    friend Result<ConfinedDirectory, std::error_code>
    open_confined_directory(const std::filesystem::path &, const std::filesystem::path &);
    friend std::optional<ConfinedEntryMetadata>
    read_confined_directory_entry(const ConfinedDirectory &, const std::filesystem::path &);
};

/// @brief Opens and verifies a stable directory context for one complete listing.
/// @param[in] shared_root Canonical filesystem boundary.
/// @param[in] directory Validated directory that will be enumerated.
/// @return Stable confined context, or the directory-level filesystem error.
[[nodiscard]] Result<ConfinedDirectory, std::error_code>
open_confined_directory(const std::filesystem::path &shared_root,
                        const std::filesystem::path &directory);

/// @brief Opens one child relative to a retained directory and reads handle-bound metadata.
/// @param[in] directory Stable context returned by open_confined_directory.
/// @param[in] native_name Single native filename relative to the directory.
/// @return Metadata, or an empty optional for a changed or unsafe entry.
[[nodiscard]] std::optional<ConfinedEntryMetadata>
read_confined_directory_entry(const ConfinedDirectory &directory,
                              const std::filesystem::path &native_name);

} // namespace sparenode::filesystem::detail
