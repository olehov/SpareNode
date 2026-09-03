#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/network_io_options.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/result.hpp"

namespace sparenode::network
{

class TcpListener;

/// @brief Owns one accepted TCP socket and releases it automatically.
///
/// Instances are not thread-safe. Do not run operations concurrently on the
/// same connection or move or destroy it until an operation has returned.
class TcpConnection final
{
  public:
    /// @brief Closes the accepted socket if this object still owns one.
    ~TcpConnection();

    /// @brief Transfers socket ownership and leaves the source object closed.
    /// @param[in,out] other Connection whose socket ownership is transferred.
    TcpConnection(TcpConnection &&other) noexcept;

    /// @brief Releases the current socket, then takes ownership from the source object.
    /// @param[in,out] other Connection whose socket ownership is transferred.
    /// @return This connection after the ownership transfer.
    TcpConnection &operator=(TcpConnection &&other) noexcept;

    /// @brief Copying is forbidden because a native socket must have one owner.
    TcpConnection(const TcpConnection &) = delete;
    /// @brief Copy assignment is forbidden because a native socket must have one owner.
    TcpConnection &operator=(const TcpConnection &) = delete;

    /// @brief Reports whether this object owns an open native socket.
    /// @return `true` while the connection owns a valid socket.
    [[nodiscard]] bool is_open() const noexcept;

    /// @brief Returns the remote endpoint.
    /// @return The peer endpoint, or no value for a moved-from connection.
    [[nodiscard]] std::optional<TcpEndpoint> peer_endpoint() const;

    /// @brief Waits for and receives at most one caller-provided buffer of bytes.
    ///
    /// A zero-byte success means that the peer performed an orderly shutdown.
    /// The operation may return fewer bytes than the buffer can hold.
    /// @param[out] buffer Destination storage for received bytes.
    /// @return The transferred byte count, or a structured network error.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer);

    /// @brief Receives bytes while allowing another thread to request cancellation.
    ///
    /// Cancellation never closes the connection. If bytes are received before
    /// the stop request is observed, that successful transfer wins the race.
    /// @param[out] buffer Destination storage for received bytes.
    /// @param[in] stop_token Token observed while waiting for socket readiness.
    /// @return The transferred byte count, or a structured network error.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer,
                                                            const std::stop_token &stop_token);

    /// @brief Receives bytes with combined cancellation and deadline control.
    /// @param[out] buffer Destination storage for received bytes.
    /// @param[in] options Stop token and optional absolute monotonic deadline.
    /// @return The transferred byte count, or a structured network error.
    [[nodiscard]] Result<std::size_t, NetworkError> receive(std::span<std::byte> buffer,
                                                            const NetworkIoOptions &options);

    /// @brief Waits for and sends at most one caller-provided buffer of bytes.
    ///
    /// A successful operation may send fewer bytes than supplied; callers that
    /// require full delivery must continue with the remaining suffix.
    /// @param[in] buffer Bytes available for transmission.
    /// @return The transferred byte count, or a structured network error.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer);

    /// @brief Sends bytes while allowing another thread to request cancellation.
    ///
    /// Cancellation never closes the connection. Bytes reported as sent remain
    /// sent even if cancellation is requested concurrently.
    /// @param[in] buffer Bytes available for transmission.
    /// @param[in] stop_token Token observed while waiting for socket readiness.
    /// @return The transferred byte count, or a structured network error.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer,
                                                         const std::stop_token &stop_token);

    /// @brief Sends bytes with combined cancellation and deadline control.
    /// @param[in] buffer Bytes available for transmission.
    /// @param[in] options Stop token and optional absolute monotonic deadline.
    /// @return The transferred byte count, or a structured network error.
    [[nodiscard]] Result<std::size_t, NetworkError> send(std::span<const std::byte> buffer,
                                                         const NetworkIoOptions &options);

  private:
    /// @brief Platform-specific implementation hidden from the public API.
    struct Impl;

    /// @brief Creates a public connection around an implementation that owns a socket.
    /// @param[in] impl Implementation whose ownership is transferred to this object.
    explicit TcpConnection(std::unique_ptr<Impl> impl) noexcept;

    /// @brief Owned platform-specific connection state, or null after a move.
    std::unique_ptr<Impl> impl_;

    // Only a listener can create a connection from a freshly accepted socket.
    friend class TcpListener;
};

} // namespace sparenode::network
