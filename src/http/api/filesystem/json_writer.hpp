#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sparenode/filesystem/directory_listing.hpp"

namespace sparenode::http::detail
{

/// @brief Appends one string using JSON escaping without changing valid UTF-8 bytes.
void append_json_string(std::string &output, std::string_view value);

/// @brief Serializes a bounded name-sorted listing without exposing native paths.
[[nodiscard]] std::string
serialize_listing(const std::vector<filesystem::DirectoryListingEntry> &entries);

} // namespace sparenode::http::detail
