#include "sparenode/network/detail/tcp_impl.hpp"

#include <memory>
#include <optional>
#include <utility>

namespace sparenode::network
{

/// Wraps an implementation that already owns an accepted native socket.
TcpConnection::TcpConnection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

/// Destroys the implementation, which closes the accepted socket through RAII.
TcpConnection::~TcpConnection() = default;

/// Transfers the implementation and leaves the source connection closed.
TcpConnection::TcpConnection(TcpConnection &&) noexcept = default;

/// Releases any current socket before taking ownership from the source connection.
TcpConnection &TcpConnection::operator=(TcpConnection &&) noexcept = default;

/// Reports whether this object currently owns a valid native socket.
bool TcpConnection::is_open() const noexcept
{
    return impl_ != nullptr && impl_->socket != detail::invalid_socket;
}

/// Returns the endpoint captured by accept, unless this object was moved from.
std::optional<TcpEndpoint> TcpConnection::peer_endpoint() const
{
    if (!is_open())
    {
        return std::nullopt;
    }

    return impl_->peer_endpoint;
}

} // namespace sparenode::network
