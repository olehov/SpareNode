#pragma once

#include <cstdint>

#include "sparenode/configuration/directives/parsed_config_value.hpp"
#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration::directives
{

/// @brief Identifies a directive permitted directly inside a filesystem `location` block.
enum class LocationDirectiveKind : std::uint8_t
{
    path,              ///< Filesystem-root path string.
    read_permission,   ///< Location read-permission switch.
    write_permission,  ///< Location write-permission switch.
    delete_permission, ///< Location delete-permission switch.
};

/// @brief Represents one syntactically valid directive inside a `location` block.
struct ParsedLocationDirective
{
    LocationDirectiveKind kind{}; ///< Recognized location-directive name.
    ParsedConfigValue value;      ///< Typed value accepted by the directive grammar.
    SourceLocation location;      ///< Position of the directive name.
};

} // namespace sparenode::configuration::directives
