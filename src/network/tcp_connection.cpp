#include "sparenode/network/detail/tcp_impl.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>

#include "sparenode/network/detail/connection_io.hpp"

namespace sparenode::network
{

// Wraps an implementation that already owns an accepted native socket.
TcpConnection::TcpConnection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

// Destroys the implementation, which closes the accepted socket through RAII.
TcpConnection::~TcpConnection() = default;

// Transfers the implementation and leaves the source connection closed.
TcpConnection::TcpConnection(TcpConnection &&) noexcept = default;

// Releases any current socket before taking ownership from the source connection.
TcpConnection &TcpConnection::operator=(TcpConnection &&) noexcept = default;

// Reports whether this object currently owns a valid native socket.
bool TcpConnection::is_open() const noexcept
{
    return impl_ != nullptr && impl_->socket != detail::invalid_socket;
}

// Returns the endpoint captured by accept, unless this object was moved from.
std::optional<TcpEndpoint> TcpConnection::peer_endpoint() const
{
    if (!is_open())
    {
        return std::nullopt;
    }

    return impl_->peer_endpoint;
}

// Receives bytes without allocating a cancellation wake channel.
Result<std::size_t, NetworkError> TcpConnection::receive(const std::span<std::byte> buffer)
{
    return receive(buffer, std::stop_token{});
}

// Waits cooperatively for readable data, then performs one nonblocking receive.
Result<std::size_t, NetworkError> TcpConnection::receive(const std::span<std::byte> buffer,
                                                         const std::stop_token &stop_token)
{
    if (!is_open())
    {
        return unexpected(NetworkError{NetworkOperation::receive, NetworkErrorDomain::state, 1});
    }
    if (buffer.empty())
    {
        return unexpected(
            NetworkError{NetworkOperation::receive, NetworkErrorDomain::validation, 1});
    }

    return impl_->io.receive(buffer, stop_token);
}

// Sends bytes without allocating a cancellation wake channel.
Result<std::size_t, NetworkError> TcpConnection::send(const std::span<const std::byte> buffer)
{
    return send(buffer, std::stop_token{});
}

// Waits cooperatively for capacity, then performs one nonblocking send.
Result<std::size_t, NetworkError> TcpConnection::send(const std::span<const std::byte> buffer,
                                                      const std::stop_token &stop_token)
{
    if (!is_open())
    {
        return unexpected(NetworkError{NetworkOperation::send, NetworkErrorDomain::state, 1});
    }
    if (buffer.empty())
    {
        return unexpected(NetworkError{NetworkOperation::send, NetworkErrorDomain::validation, 1});
    }

    return impl_->io.send(buffer, stop_token);
}

} // namespace sparenode::network
