#pragma once

#include "sparenode/application/command_line.hpp"

namespace sparenode::app
{

/// @brief Builds and runs one interactive SpareNode server process.
/// @param[in] options Validated process startup options.
/// @return Process exit status: zero after orderly shutdown, otherwise a startup failure code.
[[nodiscard]] int run_server(const application::StartupOptions &options);

} // namespace sparenode::app
