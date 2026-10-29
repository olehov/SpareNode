#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sparenode::text::ip
{

/// @brief Stores one parsed numeric IP address without platform socket types.
struct NumericAddress
{
    std::array<std::uint8_t, 16> bytes{}; ///< Network-order bytes; IPv4 uses the first four.
    bool ipv4{};                          ///< Whether the address uses IPv4 representation.
};

/// @brief Parses strict dotted-decimal IPv4 or RFC-compatible IPv6 text.
/// @param[in] value Address text without brackets, port, or zone identifier.
/// @return Canonical network-order bytes, or no value for malformed text.
[[nodiscard]] std::optional<NumericAddress> parse_numeric_address(std::string_view value) noexcept;

/// @brief Reports whether text is a strict numeric IPv4 address.
[[nodiscard]] bool is_valid_ipv4_address(std::string_view value) noexcept;

/// @brief Reports whether text is a supported numeric IPv6 address.
[[nodiscard]] bool is_valid_ipv6_address(std::string_view value) noexcept;

/// @brief Reports whether a valid numeric address belongs to its loopback range.
[[nodiscard]] bool is_loopback_address(const NumericAddress &address) noexcept;

/// @brief Reports whether every significant address byte is zero.
[[nodiscard]] bool is_unspecified_address(const NumericAddress &address) noexcept;

} // namespace sparenode::text::ip
