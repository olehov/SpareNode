#pragma once

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/tcp_endpoint.hpp"

namespace sparenode::network::detail
{

/// Owns the linked list allocated by getaddrinfo.
class AddressInfo final
{
  public:
    explicit AddressInfo(addrinfo *value) noexcept;
    ~AddressInfo();

    AddressInfo(const AddressInfo &) = delete;
    AddressInfo &operator=(const AddressInfo &) = delete;

    /// Returns the first address candidate without transferring ownership.
    [[nodiscard]] addrinfo *get() const noexcept;

  private:
    addrinfo *value_{};
};

/// Converts a native IPv4/IPv6 address into a platform-independent endpoint.
[[nodiscard]] Result<TcpEndpoint, NetworkError> endpoint_from_address(const sockaddr *address,
                                                                      NetworkOperation operation);

} // namespace sparenode::network::detail
