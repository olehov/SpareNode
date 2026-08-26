#pragma once

#include <cstdint>

#include "sparenode/configuration/directives/parsed_config_value.hpp"
#include "sparenode/configuration/source_location.hpp"

namespace sparenode::configuration::directives
{

/// @brief Identifies a directive permitted directly inside a version-one `server` block.
enum class ServerDirectiveKind : std::uint8_t
{
    bind,           ///< Server address string.
    port,           ///< Server port integer.
    multithreading, ///< Worker-pool enable switch.
    worker_threads, ///< Requested worker count.
    log_level,      ///< Minimum logging severity string.
};

/// @brief Represents one syntactically valid directive inside a `server` block.
struct ParsedServerDirective
{
    ServerDirectiveKind kind{}; ///< Recognized server-directive name.
    ParsedConfigValue value;    ///< Typed value accepted by the directive grammar.
    SourceLocation location;    ///< Position of the directive name.
};

} // namespace sparenode::configuration::directives
