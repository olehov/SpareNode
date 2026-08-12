#pragma once

#include <utility>

#include "sparenode/network/detail/native_socket.hpp"

namespace sparenode::network::detail
{

/// Temporarily owns a native socket until ownership is committed elsewhere.
class NativeSocketOwner final
{
  public:
    explicit NativeSocketOwner(const NativeSocket socket = invalid_socket) noexcept
        : socket_(socket)
    {
    }

    ~NativeSocketOwner()
    {
        close_socket(socket_);
    }

    NativeSocketOwner(const NativeSocketOwner &) = delete;
    NativeSocketOwner &operator=(const NativeSocketOwner &) = delete;

    NativeSocketOwner(NativeSocketOwner &&other) noexcept
        : socket_(std::exchange(other.socket_, invalid_socket))
    {
    }

    NativeSocketOwner &operator=(NativeSocketOwner &&) = delete;

    /// Borrows the owned handle without changing ownership.
    [[nodiscard]] NativeSocket get() const noexcept
    {
        return socket_;
    }

    /// Relinquishes ownership after another RAII object has adopted the handle.
    [[nodiscard]] NativeSocket release() noexcept
    {
        return std::exchange(socket_, invalid_socket);
    }

  private:
    NativeSocket socket_;
};

} // namespace sparenode::network::detail
