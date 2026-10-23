#pragma once

#include <string>

namespace sparenode::authentication
{

/// @brief Carries one authenticated identity without credential-source details.
struct Principal
{
    std::string identity; ///< Opaque stable identity used by authorization and audit code.

    /// @brief Compares opaque authenticated identities.
    /// @param[in] left First principal.
    /// @param[in] right Second principal.
    /// @return `true` when both stable identity values match.
    friend bool operator==(const Principal &left, const Principal &right) = default;
};

} // namespace sparenode::authentication
