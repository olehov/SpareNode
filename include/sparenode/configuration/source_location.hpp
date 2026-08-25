#pragma once

#include <cstddef>

namespace sparenode::configuration
{

/// @brief Identifies one decoded source position while retaining its byte offset.
struct SourceLocation
{
    std::size_t byte_offset{}; ///< Zero-based offset into the original UTF-8 input.
    std::size_t line{1};       ///< One-based source line.
    std::size_t column{1};     ///< One-based Unicode scalar column.

    /// @brief Compares every source-coordinate field.
    /// @param[in] lhs Left-hand location.
    /// @param[in] rhs Right-hand location.
    /// @return `true` when both locations identify the same source position.
    friend constexpr bool operator==(const SourceLocation &lhs,
                                     const SourceLocation &rhs) = default;
};

} // namespace sparenode::configuration
