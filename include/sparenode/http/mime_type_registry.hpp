#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sparenode/result.hpp"

namespace sparenode::http
{

/// @brief Associates one normalized filename extension with an HTTP media type.
struct MimeTypeMapping
{
    std::string extension;  ///< Extension with or without a leading dot.
    std::string media_type; ///< Parameter-free `type/subtype` value.
};

/// @brief Identifies why a MIME mapping collection is unsafe or ambiguous.
enum class MimeTypeRegistryErrorCode : std::uint8_t
{
    invalid_extension,         ///< Extension is empty, too long, or contains unsupported bytes.
    invalid_media_type,        ///< Media type is not one parameter-free `type/subtype` token pair.
    duplicate_extension,       ///< The normalized extension already has a mapping.
    too_many_entries,          ///< The configured mapping count exceeds the fixed startup bound.
    resource_allocation_failed ///< Registry-owned memory could not be allocated.
};

/// @brief Locates one invalid mapping in its input collection.
struct MimeTypeRegistryError
{
    MimeTypeRegistryErrorCode code{}; ///< Stable validation failure category.
    std::size_t mapping_index{};      ///< Zero-based mapping that caused the failure.
};

/// @brief Owns validated immutable extension-to-media-type mappings.
class MimeTypeRegistry final
{
  public:
    /// Maximum accepted mappings from one configuration file.
    static constexpr std::size_t maximum_entries = 4096;

    /// Maximum extension length excluding its optional leading dot.
    static constexpr std::size_t maximum_extension_bytes = 32;

    /// Maximum accepted parameter-free media-type length.
    static constexpr std::size_t maximum_media_type_bytes = 127;

    /// @brief Creates an empty registry that safely falls back for every extension.
    MimeTypeRegistry() = default;

    /// @brief Validates, normalizes, and sorts MIME mappings for immutable lookup.
    /// @param[in] mappings User-configured mappings in source order.
    /// @return Registry or the first deterministic validation failure.
    [[nodiscard]] static Result<MimeTypeRegistry, MimeTypeRegistryError>
    create(std::vector<MimeTypeMapping> mappings);

    /// @brief Selects a safe response Content-Type from the final path extension.
    /// @param[in] path Decoded request path or filename.
    /// @return Configured type, safe text policy, or `application/octet-stream`.
    [[nodiscard]] std::string_view content_type_for_path(std::string_view path) const noexcept;

    /// @brief Returns the number of configured extension mappings.
    /// @return Count of normalized unique mappings.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return mappings_.size();
    }

  private:
    /// @brief Stores mappings after validation, normalization, and sorting.
    /// @param[in] mappings Validated mappings in extension order.
    explicit MimeTypeRegistry(std::vector<MimeTypeMapping> mappings)
        : mappings_(std::move(mappings))
    {
    }

    std::vector<MimeTypeMapping> mappings_; ///< Normalized mappings sorted by extension.
};

/// @brief Returns stable text for one MIME registry validation failure.
[[nodiscard]] const char *to_string(MimeTypeRegistryErrorCode code) noexcept;

} // namespace sparenode::http
