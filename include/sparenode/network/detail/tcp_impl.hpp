#pragma once

#include <utility>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/socket_poller.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_listener.hpp"

namespace sparenode::network
{

struct TcpConnection::Impl
{
    /// Takes ownership of an accepted socket and records its remote endpoint.
    Impl(const detail::NativeSocket socket, TcpEndpoint endpoint)
        : socket(socket), peer_endpoint(std::move(endpoint))
    {
    }

    /// Releases the accepted socket when the public connection is destroyed.
    ~Impl()
    {
        detail::close_socket(socket);
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    detail::NativeSocket socket{detail::invalid_socket};
    TcpEndpoint peer_endpoint;
};

struct TcpListener::Impl
{
    /// Takes ownership of a socket that has already been bound and made listening.
    explicit Impl(const detail::NativeSocket socket) noexcept : socket(socket)
    {
    }

    /// Releases the listening socket when the public listener is destroyed.
    ~Impl()
    {
        detail::close_socket(socket);
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    detail::NativeSocket socket{detail::invalid_socket};
    detail::NativeSocketPoller poller;
};

} // namespace sparenode::network
