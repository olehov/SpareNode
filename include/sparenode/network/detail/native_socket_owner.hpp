#pragma once

#include <utility>

#include "sparenode/network/detail/native_socket.hpp"

namespace sparenode::network::detail
{

/// @brief Temporarily owns a native socket until ownership is committed elsewhere.
class NativeSocketOwner final
{
  public:
    /// @brief Adopts a native socket handle.
    /// @param[in] socket Handle whose ownership is transferred to this object.
    explicit NativeSocketOwner(const NativeSocket socket = invalid_socket) noexcept
        : socket_(socket)
    {
    }

    /// @brief Closes the owned native socket, if any.
    ~NativeSocketOwner()
    {
        close_socket(socket_);
    }

    /// @brief Copying is forbidden because a native socket must have one owner.
    NativeSocketOwner(const NativeSocketOwner &) = delete;
    /// @brief Copy assignment is forbidden because a native socket must have one owner.
    NativeSocketOwner &operator=(const NativeSocketOwner &) = delete;

    /// @brief Transfers ownership and leaves the source empty.
    /// @param[in,out] other Owner whose socket is transferred.
    NativeSocketOwner(NativeSocketOwner &&other) noexcept
        : socket_(std::exchange(other.socket_, invalid_socket))
    {
    }

    /// @brief Move assignment is forbidden to keep cleanup paths explicit.
    NativeSocketOwner &operator=(NativeSocketOwner &&) = delete;

    /// @brief Borrows the owned handle without changing ownership.
    /// @return The owned handle, or invalid_socket when empty.
    [[nodiscard]] NativeSocket get() const noexcept
    {
        return socket_;
    }

    /// @brief Relinquishes ownership after another RAII object adopts the handle.
    /// @return The previously owned handle, or invalid_socket when empty.
    [[nodiscard]] NativeSocket release() noexcept
    {
        return std::exchange(socket_, invalid_socket);
    }

  private:
    /// @brief Solely owned native socket handle.
    NativeSocket socket_;
};

} // namespace sparenode::network::detail
