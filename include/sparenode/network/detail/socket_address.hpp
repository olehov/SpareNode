#pragma once

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/tcp_endpoint.hpp"

namespace sparenode::network::detail
{

/// @brief Owns the linked list allocated by getaddrinfo.
class AddressInfo final
{
  public:
    /// @brief Adopts an address list allocated by getaddrinfo.
    /// @param[in] value Address list whose ownership is transferred.
    explicit AddressInfo(addrinfo *value) noexcept;
    /// @brief Releases the owned address list with freeaddrinfo.
    ~AddressInfo();

    /// @brief Copying is forbidden because the address list has one owner.
    AddressInfo(const AddressInfo &) = delete;
    /// @brief Copy assignment is forbidden because the address list has one owner.
    AddressInfo &operator=(const AddressInfo &) = delete;

    /// @brief Returns the first address candidate without transferring ownership.
    /// @return Borrowed pointer valid for the lifetime of this owner.
    [[nodiscard]] addrinfo *get() const noexcept;

  private:
    /// @brief Owned head of the address list, or null when empty.
    addrinfo *value_{};
};

/// @brief Converts a native IPv4/IPv6 address into a portable endpoint.
/// @param[in] address Borrowed native address to convert.
/// @param[in] operation Public operation to record if conversion fails.
/// @return The converted endpoint, or a structured address-resolution error.
[[nodiscard]] Result<TcpEndpoint, NetworkError> endpoint_from_address(const sockaddr *address,
                                                                      NetworkOperation operation);

} // namespace sparenode::network::detail
