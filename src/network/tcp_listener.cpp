#include "sparenode/network/tcp_listener.hpp"

#include <memory>
#include <string>
#include <utility>

#include "sparenode/network/detail/native_socket.hpp"
#include "sparenode/network/detail/retry_interrupted_operation.hpp"
#include "sparenode/network/detail/socket_address.hpp"
#include "sparenode/network/detail/tcp_impl.hpp"

namespace sparenode::network
{

/// Wraps an implementation that already owns a bound, listening socket.
TcpListener::TcpListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

/// Destroys the implementation, which closes the listening socket through RAII.
TcpListener::~TcpListener() = default;

/// Transfers the implementation and leaves the source listener closed.
TcpListener::TcpListener(TcpListener &&) noexcept = default;

/// Releases any current socket before taking ownership from the source listener.
TcpListener &TcpListener::operator=(TcpListener &&) noexcept = default;

/// Resolves a numeric address, binds the first usable candidate, and starts listening.
Result<TcpListener, NetworkError> TcpListener::bind(const TcpEndpoint &endpoint, const int backlog)
{
    if (endpoint.address.empty() || backlog <= 0)
    {
        return unexpected(NetworkError{NetworkOperation::bind, NetworkErrorDomain::validation, 1});
    }

    if (auto runtime = detail::ensure_socket_runtime(); !runtime)
    {
        return unexpected(runtime.error());
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // Numeric-only resolution prevents DNS lookup and implicit interface selection.
    hints.ai_flags = AI_NUMERICHOST;

    const auto service = std::to_string(endpoint.port);
    addrinfo *resolved_addresses = nullptr;
    const int resolve_result =
        getaddrinfo(endpoint.address.c_str(), service.c_str(), &hints, &resolved_addresses);
    if (resolve_result != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::resolve_address,
            NetworkErrorDomain::address_resolution,
            resolve_result,
        });
    }

    const detail::AddressInfo addresses(resolved_addresses);
    NetworkError last_error{
        NetworkOperation::create_socket,
        NetworkErrorDomain::socket,
        0,
    };

    for (auto *current = addresses.get(); current != nullptr; current = current->ai_next)
    {
        const detail::NativeSocket socket_handle =
            ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socket_handle == detail::invalid_socket)
        {
            last_error = NetworkError{
                NetworkOperation::create_socket,
                NetworkErrorDomain::socket,
                detail::last_socket_error(),
            };
            continue;
        }

        if (!detail::configure_socket_security({socket_handle, current->ai_family}))
        {
            last_error = NetworkError{
                NetworkOperation::configure_socket,
                NetworkErrorDomain::socket,
                detail::last_socket_error(),
            };
            detail::close_socket(socket_handle);
            continue;
        }

        if (::bind(socket_handle, current->ai_addr,
                   static_cast<detail::SocketLength>(current->ai_addrlen)) != 0)
        {
            last_error = NetworkError{
                NetworkOperation::bind,
                NetworkErrorDomain::socket,
                detail::last_socket_error(),
            };
            detail::close_socket(socket_handle);
            continue;
        }

        if (::listen(socket_handle, backlog) != 0)
        {
            last_error = NetworkError{
                NetworkOperation::listen,
                NetworkErrorDomain::socket,
                detail::last_socket_error(),
            };
            detail::close_socket(socket_handle);
            continue;
        }

        return TcpListener(std::make_unique<Impl>(socket_handle));
    }

    return unexpected(last_error);
}

/// Waits for a client and returns exclusive ownership of the accepted connection.
Result<TcpConnection, NetworkError> TcpListener::accept()
{
    if (!is_open())
    {
        return unexpected(NetworkError{NetworkOperation::accept, NetworkErrorDomain::state, 1});
    }

    sockaddr_storage peer_address{};
    detail::SocketLength peer_address_length{};
    const auto accept_result = detail::retry_interrupted_operation(
        detail::invalid_socket,
        [this, &peer_address, &peer_address_length]
        {
            peer_address_length = static_cast<detail::SocketLength>(sizeof(peer_address));
            return ::accept(impl_->socket, reinterpret_cast<sockaddr *>(&peer_address),
                            &peer_address_length);
        },
        detail::last_socket_error, detail::should_retry_accept);
    if (accept_result.value == detail::invalid_socket)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            accept_result.error_code,
        });
    }

    const detail::NativeSocket accepted_socket = accept_result.value;
    auto endpoint = detail::endpoint_from_address(reinterpret_cast<const sockaddr *>(&peer_address),
                                                  NetworkOperation::query_peer_endpoint);
    if (!endpoint)
    {
        detail::close_socket(accepted_socket);
        return unexpected(endpoint.error());
    }

    return TcpConnection(
        std::make_unique<TcpConnection::Impl>(accepted_socket, std::move(endpoint.value())));
}

/// Queries the effective bound address, including an OS-selected ephemeral port.
Result<TcpEndpoint, NetworkError> TcpListener::local_endpoint() const
{
    if (!is_open())
    {
        return unexpected(NetworkError{
            NetworkOperation::query_local_endpoint,
            NetworkErrorDomain::state,
            1,
        });
    }

    sockaddr_storage local_address{};
    detail::SocketLength local_address_length = sizeof(local_address);
    if (getsockname(impl_->socket, reinterpret_cast<sockaddr *>(&local_address),
                    &local_address_length) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::query_local_endpoint,
            NetworkErrorDomain::socket,
            detail::last_socket_error(),
        });
    }

    return detail::endpoint_from_address(reinterpret_cast<const sockaddr *>(&local_address),
                                         NetworkOperation::query_local_endpoint);
}

/// Reports whether this object currently owns a valid listening socket.
bool TcpListener::is_open() const noexcept
{
    return impl_ != nullptr && impl_->socket != detail::invalid_socket;
}

} // namespace sparenode::network
