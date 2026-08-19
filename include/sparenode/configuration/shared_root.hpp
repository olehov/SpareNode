#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>

#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies why a shared-root path could not be accepted.
enum class SharedRootErrorCode : std::uint8_t
{
    empty_path,              ///< No filesystem path was supplied.
    not_found,               ///< The supplied path does not exist.
    not_directory,           ///< The supplied path does not identify a directory.
    canonicalization_failed, ///< The operating system could not resolve the path safely.
};

/// @brief Describes a failure while validating the configured shared root.
struct SharedRootError
{
    SharedRootErrorCode code;          ///< Portable category of the validation failure.
    std::filesystem::path input_path;  ///< Original path supplied by the caller.
    std::error_code system_error = {}; ///< Optional platform filesystem error.
};

/// @brief Represents the canonical directory that bounds all shared filesystem access.
///
/// Instances can only be obtained through create(), so every stored path identifies an
/// existing directory and is represented canonically.
class SharedRoot final
{
  public:
    /// @brief Validates and canonicalizes a candidate shared-root directory.
    /// @param[in] input_path Filesystem path supplied by the application user.
    /// @return A validated shared root, or a structured validation error.
    [[nodiscard]] static Result<SharedRoot, SharedRootError>
    create(const std::filesystem::path &input_path);

    /// @brief Returns the canonical absolute path of the shared directory.
    /// @return A stable path reference valid for the lifetime of this object.
    [[nodiscard]] const std::filesystem::path &path() const noexcept;

  private:
    /// @brief Creates a validated instance from an already canonical directory path.
    /// @param[in] canonical_path Canonical absolute directory path to store.
    explicit SharedRoot(std::filesystem::path canonical_path);

    std::filesystem::path path_; ///< Canonical filesystem security boundary.
};

/// @brief Returns a concise description of a shared-root validation failure.
/// @param[in] code Portable error category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(SharedRootErrorCode code) noexcept;

} // namespace sparenode::configuration
