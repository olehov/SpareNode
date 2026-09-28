#pragma once

#include <string>
#include <vector>

#include "sparenode/configuration/directives/parsed_location_directive.hpp"
#include "sparenode/configuration/directives/parsed_server_directive.hpp"
#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration
{

/// @brief Represents one parsed filesystem-backed `location` block.
struct ParsedLocationBlock
{
    std::string api_path;                  ///< Once-decoded public HTTP path prefix.
    SourceLocation location;               ///< Position of the `location` keyword.
    SourceLocation api_path_location;      ///< Position of the API path literal.
    SourceLocation closing_brace_location; ///< Position of the closing block delimiter.
    std::vector<directives::ParsedLocationDirective> directives; ///< Directives in source order.
};

/// @brief Represents the parsed top-level `server` block.
struct ParsedServerBlock
{
    SourceLocation location;               ///< Position of the `server` keyword.
    SourceLocation closing_brace_location; ///< Position of the closing block delimiter.
    std::vector<directives::ParsedServerDirective> directives; ///< Server directives in order.
    std::vector<ParsedLocationBlock> locations; ///< Filesystem locations in source order.
};

/// @brief Owns the complete syntactic representation of one configuration document.
struct ParsedConfiguration
{
    ParsedServerBlock server;             ///< Sole grammatical top-level block.
    SourceLocation end_of_input_location; ///< Position immediately after the document.
};

} // namespace sparenode::configuration
