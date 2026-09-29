#pragma once

#include <string_view>

namespace sparenode::http::detail
{

/// @brief Validates the supported ASCII HTTP Host authority subset without allocation.
///
/// Accepted hosts are DNS-style names, strict dotted-decimal IPv4 addresses, and bracketed IPv6
/// literals. Each form may include a decimal port from 0 through 65535.
/// @param[in] authority Trimmed Host field value.
/// @return `true` when the complete value belongs to the supported authority grammar.
[[nodiscard]] bool is_valid_host_authority(std::string_view authority) noexcept;

} // namespace sparenode::http::detail
