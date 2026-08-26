#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration::directives
{

/// @brief Stores one parsed scalar without coupling syntax to runtime configuration classes.
using ParsedConfigScalar = std::variant<std::string, std::uint64_t, bool>;

/// @brief Preserves a directive value together with the location of its literal.
struct ParsedConfigValue
{
    ParsedConfigScalar scalar; ///< Once-decoded and type-converted syntactic value.
    SourceLocation location;   ///< Position of the value token.
};

} // namespace sparenode::configuration::directives
