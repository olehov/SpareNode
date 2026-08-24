#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sparenode/result.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Identifies why an encoded network path could not be decoded safely.
enum class PathRequestDecodeError : std::uint8_t
{
    invalid_percent_encoding ///< A percent escape is malformed or remains ambiguously encoded.
};

/// @brief Decodes one URL path segment sequence and normalizes portable separators.
///
/// Percent escapes are decoded exactly once. A decoded result that still contains
/// another valid percent escape is rejected so no downstream double-decoding step
/// can reinterpret the validated path. Both slash styles become `/` before native
/// filesystem parsing, providing identical traversal semantics on Windows and Linux.
///
/// @param[in] requested_path Raw byte sequence received from an untrusted client.
/// @return Decoded path with portable separators, or a structured encoding error.
[[nodiscard]] Result<std::string, PathRequestDecodeError>
decode_path_request(std::string_view requested_path);

} // namespace sparenode::filesystem::detail
