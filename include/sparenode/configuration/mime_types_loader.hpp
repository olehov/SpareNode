#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "sparenode/http/mime_type_registry.hpp"
#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies why a configured `mime.types` file could not be loaded.
enum class MimeTypesLoadErrorCode : std::uint8_t
{
    open_failed,               ///< The selected file could not be opened.
    read_failed,               ///< The selected file could not be read completely.
    file_too_large,            ///< File exceeds the fixed startup input bound.
    line_too_long,             ///< One physical line exceeds the parser bound.
    invalid_line,              ///< A non-comment line lacks a type or extension.
    invalid_extension,         ///< An extension token is unsupported.
    invalid_media_type,        ///< A media-type token is unsupported.
    duplicate_extension,       ///< An extension appears more than once.
    too_many_entries,          ///< Expanded extension mapping count exceeds the bound.
    resource_allocation_failed ///< Parser-owned memory could not be allocated.
};

/// @brief Preserves the source line associated with a MIME loading failure.
struct MimeTypesLoadError
{
    MimeTypesLoadErrorCode code{}; ///< Stable loading or parsing category.
    std::size_t line{1};           ///< One-based source line, or one for file-level failures.
};

/// @brief Loads a bounded conventional `mime.types` file into an immutable registry.
class MimeTypesLoader final
{
  public:
    /// Maximum MIME source bytes accepted at startup.
    static constexpr std::size_t maximum_file_bytes = std::size_t{256} * 1024;

    /// Maximum physical line length excluding its line ending.
    static constexpr std::size_t maximum_line_bytes = 1024;

    /// @brief Reads and parses one MIME mapping file.
    /// @param[in] source_path File selected by the validated server directive.
    /// @return Immutable registry or a source-located loading failure.
    [[nodiscard]] static Result<http::MimeTypeRegistry, MimeTypesLoadError>
    load(const std::filesystem::path &source_path);

    /// @brief Parses bounded MIME source text without filesystem access.
    /// @param[in] source Complete source bytes retained for the duration of parsing.
    /// @return Immutable registry or a source-located syntax or limit failure.
    [[nodiscard]] static Result<http::MimeTypeRegistry, MimeTypesLoadError>
    parse(std::string_view source);
};

/// @brief Returns stable text for one MIME loading failure.
[[nodiscard]] const char *to_string(MimeTypesLoadErrorCode code) noexcept;

} // namespace sparenode::configuration
