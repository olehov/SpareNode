#pragma once

#include <cstdint>

#include "sparenode/configuration/directives/parsed_config_value.hpp"
#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration::directives
{

/// @brief Identifies a directive permitted directly inside a version-one `share` block.
enum class ShareDirectiveKind : std::uint8_t
{
    path,              ///< Shared-directory path string.
    read_permission,   ///< Share read-permission switch.
    write_permission,  ///< Share write-permission switch.
    delete_permission, ///< Share delete-permission switch.
};

/// @brief Represents one syntactically valid directive inside a `share` block.
struct ParsedShareDirective
{
    ShareDirectiveKind kind{}; ///< Recognized share-directive name.
    ParsedConfigValue value;   ///< Typed value accepted by the directive grammar.
    SourceLocation location;   ///< Position of the directive name.
};

} // namespace sparenode::configuration::directives
