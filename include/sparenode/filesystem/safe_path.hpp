#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/result.hpp"

namespace sparenode::filesystem
{

/// @brief Identifies why an untrusted path could not be confined to the shared root.
enum class SafePathErrorCode : std::uint8_t
{
    invalid_encoding,    ///< The requested path is not valid UTF-8.
    embedded_null,       ///< The requested path contains an ambiguous null byte.
    rooted_path,         ///< The requested path supplies a root, drive, or absolute location.
    outside_shared_root, ///< The resolved path is not contained by the configured shared root.
};

/// @brief Describes a failure while resolving an untrusted path.
struct SafePathError
{
    SafePathErrorCode code = SafePathErrorCode::invalid_encoding; ///< Portable error category.
    std::string requested_path; ///< Original UTF-8 path supplied by the caller.
};

/// @brief Represents a filesystem path resolved and confined to one shared root.
///
/// Instances can only be obtained through resolve(). The stored path is absolute and
/// lexically normalized, so a missing final component remains representable for future
/// create or upload operations. Symbolic-link and reparse-point confinement is added by
/// the dedicated filesystem-security layers before untrusted paths reach file access.
class SafePath final
{
  public:
    /// @brief Resolves an untrusted UTF-8 path relative to a validated shared root.
    /// @param[in] shared_root Root that must contain the resolved candidate.
    /// @param[in] requested_path Relative UTF-8 path received from an untrusted caller.
    /// @return A confined path, or a structured validation error.
    /// @note An empty requested path resolves to the shared root itself.
    [[nodiscard]] static Result<SafePath, SafePathError>
    resolve(const configuration::SharedRoot &shared_root, std::string_view requested_path);

    /// @brief Returns the absolute lexically normalized filesystem path.
    /// @return A stable path reference valid for the lifetime of this object.
    [[nodiscard]] const std::filesystem::path &path() const noexcept;

  private:
    /// @brief Creates an instance from a path already confined to its shared root.
    /// @param[in] resolved_path Absolute lexically normalized path to store.
    explicit SafePath(std::filesystem::path resolved_path);

    std::filesystem::path path_; ///< Validated path consumed by filesystem operations.
};

/// @brief Returns a concise description of a safe-path validation failure.
/// @param[in] code Portable error category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(SafePathErrorCode code) noexcept;

} // namespace sparenode::filesystem
