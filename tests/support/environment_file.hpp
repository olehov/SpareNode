#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

#include "support/temporary_directory.hpp"

namespace sparenode::test
{

/// @brief Writes test configuration to a `.env` file inside a temporary directory.
/// @param[in] directory Temporary directory that owns the environment file.
/// @param[in] content Complete environment-file content to write.
/// @return Path to the created `.env` file.
[[nodiscard]] inline std::filesystem::path
write_environment_file(const TemporaryDirectory &directory, const std::string_view content)
{
    const auto environment_file = directory.path() / ".env";
    std::ofstream(environment_file) << content;
    return environment_file;
}

} // namespace sparenode::test
