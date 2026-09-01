#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "sparenode/result.hpp"

namespace sparenode::configuration
{

/// @brief Identifies why an environment file could not be loaded.
enum class EnvironmentFileErrorCode : std::uint8_t
{
    file_not_found,    ///< The requested environment file does not exist.
    file_unreadable,   ///< The environment file could not be read completely.
    malformed_entry,   ///< A non-comment line is not a valid key-value assignment.
    duplicate_variable ///< The same variable name occurs more than once.
};

/// @brief Describes a syntax or I/O failure while loading an environment file.
struct EnvironmentFileError
{
    EnvironmentFileErrorCode code =
        EnvironmentFileErrorCode::file_unreadable; ///< Portable error category.
    std::filesystem::path source_path;             ///< Environment file being processed.
    std::size_t line_number = 0;                   ///< One-based source line, or zero.
    std::string variable;                          ///< Variable associated with the failure.
};

/// @brief Stores every key-value assignment parsed from one environment file.
class EnvironmentFile final
{
  public:
    /// @brief Loads the default `.env` file from the process working directory.
    /// @return All parsed variables, or a structured file error.
    [[nodiscard]] static Result<EnvironmentFile, EnvironmentFileError> load_default();

    /// @brief Loads all assignments from an environment file.
    /// @param[in] source_path Path to the UTF-8 environment file to read.
    /// @return All parsed variables, or a structured file error.
    [[nodiscard]] static Result<EnvironmentFile, EnvironmentFileError>
    load(const std::filesystem::path &source_path);

    /// @brief Finds a parsed environment variable without copying its value.
    /// @param[in] variable Variable name to look up exactly.
    /// @return Borrowed value, or no value when the variable is absent.
    /// @warning A returned view remains valid only while this object is alive and unmoved.
    [[nodiscard]] std::optional<std::string_view> find(std::string_view variable) const noexcept;

    /// @brief Reports how many distinct assignments were loaded.
    /// @return Number of stored environment variables.
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    /// @brief Ordered owning storage for distinct environment assignments.
    using Variables = std::map<std::string, std::string, std::less<>>;

    /// @brief Default environment-file name used by load_default().
    static constexpr std::string_view default_file_name = ".env";

    /// @brief Creates an environment file from fully parsed assignments.
    /// @param[in] variables Distinct environment variables to take ownership of.
    explicit EnvironmentFile(Variables variables);

    Variables variables_; ///< Every distinct assignment parsed from the source file.
};

/// @brief Returns a concise description of an environment-file failure.
/// @param[in] code Portable error category to describe.
/// @return Static English text suitable for diagnostics.
[[nodiscard]] const char *to_string(EnvironmentFileErrorCode code) noexcept;

} // namespace sparenode::configuration
