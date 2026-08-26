#pragma once

#include <string>
#include <vector>

#include "sparenode/configuration/directives/parsed_server_directive.hpp"
#include "sparenode/configuration/directives/parsed_share_directive.hpp"
#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration
{

/// @brief Represents one parsed `share` block without applying semantic cardinality rules.
struct ParsedShareBlock
{
    std::string name;                      ///< Once-decoded display name.
    SourceLocation location;               ///< Position of the `share` keyword.
    SourceLocation name_location;          ///< Position of the name literal.
    SourceLocation closing_brace_location; ///< Position of the closing block delimiter.
    std::vector<directives::ParsedShareDirective> directives; ///< Directives in source order.
};

/// @brief Represents the parsed top-level `server` block.
struct ParsedServerBlock
{
    SourceLocation location;               ///< Position of the `server` keyword.
    SourceLocation closing_brace_location; ///< Position of the closing block delimiter.
    std::vector<directives::ParsedServerDirective> directives; ///< Server directives in order.
    std::vector<ParsedShareBlock> shares;                      ///< Share blocks in source order.
};

/// @brief Owns the complete syntactic representation of one configuration document.
struct ParsedConfiguration
{
    ParsedServerBlock server;             ///< Sole grammatical top-level block.
    SourceLocation end_of_input_location; ///< Position immediately after the document.
};

} // namespace sparenode::configuration
