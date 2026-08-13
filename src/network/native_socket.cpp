#include "sparenode/network/detail/native_socket.hpp"

#include <cerrno>

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

/// Balances the process-wide Winsock startup and cleanup calls through RAII.
class WinsockRuntime final
{
  public:
    WinsockRuntime() noexcept
    {
        WSADATA data{};
        result_ = WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~WinsockRuntime()
    {
        if (result_ == 0)
        {
            static_cast<void>(WSACleanup());
        }
    }

    WinsockRuntime(const WinsockRuntime &) = delete;
    WinsockRuntime &operator=(const WinsockRuntime &) = delete;

    [[nodiscard]] int result() const noexcept
    {
        return result_;
    }

  private:
    int result_{};
};

} // namespace

/// Reads the thread-local Winsock error left by the most recent failed call.
int last_socket_error() noexcept
{
    return WSAGetLastError();
}

/// Closes an owned Windows socket while keeping destructors non-throwing.
void close_socket(const NativeSocket socket) noexcept
{
    if (socket != invalid_socket)
    {
        static_cast<void>(closesocket(socket));
    }
}

/// Starts Winsock once for the lifetime of the process.
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

/// Prevents port sharing and makes IPv6 binding behaviour explicit on Windows.
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

/// Enables nonblocking operations without changing any other socket setting.
bool configure_socket_nonblocking(const NativeSocket socket) noexcept
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

/// Restores blocking operations on a socket accepted from a nonblocking listener.
bool configure_socket_blocking(const NativeSocket socket) noexcept
{
    u_long enabled = 0;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

/// Recognizes the Winsock result for a nonblocking operation with no available work.
bool socket_error_would_block(const int error_code) noexcept
{
    return error_code == WSAEWOULDBLOCK;
}

#else

/// Reads errno immediately after a failed POSIX socket operation.
int last_socket_error() noexcept
{
    return errno;
}

/// Closes an owned file descriptor while keeping destructors non-throwing.
void close_socket(const NativeSocket socket) noexcept
{
    if (socket != invalid_socket)
    {
        static_cast<void>(::close(socket));
    }
}

/// Confirms that no runtime initialization is necessary on POSIX systems.
Result<void, NetworkError> ensure_socket_runtime()
{
    // POSIX sockets require no process-wide initialization.
    return {};
}

/// Enables safe rebinding after restart and restricts IPv6 listeners to IPv6.
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

/// Enables O_NONBLOCK while preserving all existing descriptor flags.
bool configure_socket_nonblocking(const NativeSocket socket) noexcept
{
    const int current_flags = fcntl(socket, F_GETFL, 0);
    return current_flags >= 0 && fcntl(socket, F_SETFL, current_flags | O_NONBLOCK) == 0;
}

/// Clears O_NONBLOCK while preserving all unrelated descriptor flags.
bool configure_socket_blocking(const NativeSocket socket) noexcept
{
    const int current_flags = fcntl(socket, F_GETFL, 0);
    return current_flags >= 0 && fcntl(socket, F_SETFL, current_flags & ~O_NONBLOCK) == 0;
}

/// Recognizes either POSIX spelling for a temporarily unavailable operation.
bool socket_error_would_block(const int error_code) noexcept
{
    return error_code == EAGAIN || error_code == EWOULDBLOCK;
}

#endif

} // namespace sparenode::network::detail
