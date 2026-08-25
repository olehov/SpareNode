#include "sparenode/network/detail/native_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace sparenode::network::detail
{

#ifdef _WIN32
namespace
{

/// @brief Balances process-wide Winsock startup and cleanup through RAII.
class WinsockRuntime final
{
  public:
    /// @brief Requests Winsock 2.2 and stores the startup result code.
    WinsockRuntime() noexcept
    {
        WSADATA data{};
        result_ = WSAStartup(MAKEWORD(2, 2), &data);
    }

    /// @brief Cleans up Winsock only when startup succeeded.
    ~WinsockRuntime()
    {
        if (result_ == 0)
        {
            static_cast<void>(WSACleanup());
        }
    }

    /// @brief Copying is forbidden because startup has one cleanup owner.
    WinsockRuntime(const WinsockRuntime &) = delete;
    /// @brief Copy assignment is forbidden because startup has one cleanup owner.
    WinsockRuntime &operator=(const WinsockRuntime &) = delete;

    /// @brief Returns the result produced by WSAStartup.
    /// @return Zero after successful initialization, otherwise a Winsock error code.
    [[nodiscard]] int result() const noexcept
    {
        return result_;
    }

  private:
    /// @brief Cached WSAStartup result used by callers and cleanup.
    int result_{};
};

} // namespace

// Reads the thread-local Winsock error left by the most recent failed call.
int last_socket_error() noexcept
{
    return WSAGetLastError();
}

// Closes an owned Windows socket while keeping destructors non-throwing.
void close_socket(const NativeSocket socket) noexcept
{
    if (socket != invalid_socket)
    {
        static_cast<void>(closesocket(socket));
    }
}

// Starts Winsock once for the lifetime of the process.
Result<void, NetworkError> ensure_socket_runtime()
{
    // Function-local static initialization is thread-safe and runs only once.
    static const WinsockRuntime runtime;
    if (runtime.result() != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::initialize,
            NetworkErrorDomain::socket,
            runtime.result(),
        });
    }

    return {};
}

// Prevents port sharing and makes IPv6 binding behaviour explicit on Windows.
bool configure_socket_security(const SocketConfiguration configuration)
{
    // Windows requires exclusive ownership to prevent another process from
    // binding the same listener address and port.
    const BOOL exclusive = TRUE;
    if (setsockopt(configuration.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive),
                   static_cast<int>(sizeof(exclusive))) != 0)
    {
        return false;
    }

    if (configuration.family == AF_INET6)
    {
        // Avoid an implicit dual-stack listener; exposed interfaces stay explicit.
        const DWORD ipv6_only = 1;
        if (setsockopt(configuration.socket, IPPROTO_IPV6, IPV6_V6ONLY,
                       reinterpret_cast<const char *>(&ipv6_only),
                       static_cast<int>(sizeof(ipv6_only))) != 0)
        {
            return false;
        }
    }

    return true;
}

// Enables nonblocking operations without changing any other socket setting.
bool configure_socket_nonblocking(const NativeSocket socket) noexcept
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

// Recognizes the Winsock result for a nonblocking operation with no available work.
bool socket_error_would_block(const int error_code) noexcept
{
    return error_code == WSAEWOULDBLOCK;
}

// Recognizes a Winsock operation interrupted before it transferred bytes.
bool socket_error_interrupted(const int error_code) noexcept
{
    return error_code == WSAEINTR;
}

// Adapts the bounded span length to Winsock's signed integer API.
std::ptrdiff_t receive_socket(const NativeSocket socket, std::span<std::byte> buffer) noexcept
{
    const auto size = static_cast<int>(
        (std::min)(buffer.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    return ::recv(socket, reinterpret_cast<char *>(buffer.data()), size, 0);
}

// Sends one bounded chunk through Winsock.
std::ptrdiff_t send_socket(const NativeSocket socket,
                           const std::span<const std::byte> buffer) noexcept
{
    const auto size = static_cast<int>(
        (std::min)(buffer.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    return ::send(socket, reinterpret_cast<const char *>(buffer.data()), size, 0);
}

#else

// Reads errno immediately after a failed POSIX socket operation.
int last_socket_error() noexcept
{
    return errno;
}

// Closes an owned file descriptor while keeping destructors non-throwing.
void close_socket(const NativeSocket socket) noexcept
{
    if (socket != invalid_socket)
    {
        static_cast<void>(::close(socket));
    }
}

// Confirms that no runtime initialization is necessary on POSIX systems.
Result<void, NetworkError> ensure_socket_runtime()
{
    // POSIX sockets require no process-wide initialization.
    return {};
}

// Enables safe rebinding after restart and restricts IPv6 listeners to IPv6.
bool configure_socket_security(const SocketConfiguration configuration)
{
    // Permit rebinding after restart while connections from the previous run
    // remain in TIME_WAIT. This does not allow a second live TCP listener.
    const int reuse_address = 1;
    if (setsockopt(configuration.socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) != 0)
    {
        return false;
    }

    if (configuration.family != AF_INET6)
    {
        return true;
    }

    const int ipv6_only = 1;
    return setsockopt(configuration.socket, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only,
                      sizeof(ipv6_only)) == 0;
}

// Enables O_NONBLOCK while preserving all existing descriptor flags.
bool configure_socket_nonblocking(const NativeSocket socket) noexcept
{
    const int current_flags = fcntl(socket, F_GETFL, 0);
    return current_flags >= 0 && fcntl(socket, F_SETFL, current_flags | O_NONBLOCK) == 0;
}

// Recognizes either POSIX spelling for a temporarily unavailable operation.
bool socket_error_would_block(const int error_code) noexcept
{
    return error_code == EAGAIN || error_code == EWOULDBLOCK;
}

// Recognizes a POSIX operation interrupted before it transferred bytes.
bool socket_error_interrupted(const int error_code) noexcept
{
    return error_code == EINTR;
}

// Receives one caller-bounded chunk from a POSIX socket.
std::ptrdiff_t receive_socket(const NativeSocket socket, std::span<std::byte> buffer) noexcept
{
    return ::recv(socket, buffer.data(), buffer.size(), 0);
}

// Suppresses SIGPIPE so a disconnected peer is reported as a structured error.
std::ptrdiff_t send_socket(const NativeSocket socket,
                           const std::span<const std::byte> buffer) noexcept
{
#ifndef MSG_NOSIGNAL
#error "send_socket requires MSG_NOSIGNAL; add SO_NOSIGPIPE support for this platform"
#endif
    constexpr int send_flags = MSG_NOSIGNAL;
    return ::send(socket, buffer.data(), buffer.size(), send_flags);
}

#endif

} // namespace sparenode::network::detail
