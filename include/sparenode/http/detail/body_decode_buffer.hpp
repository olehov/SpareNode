#pragma once

#include <cstddef>

namespace sparenode::http::detail
{
/// @brief Scratch-buffer capacity for body decoding, independent of request and chunk limits.
inline constexpr std::size_t body_decode_buffer_bytes = std::size_t{16} * 1024;
} // namespace sparenode::http::detail
