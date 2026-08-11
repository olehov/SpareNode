#include "sparenode/network/tcp_listener.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sparenode::network
{
namespace
{
// Everything in this unnamed namespace is private to this translation unit. It
// contains the platform-specific layer hidden behind the public C++ API.

#ifdef _WIN32
// Windows represents sockets with SOCKET and uses int for address lengths.
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;

struct SocketConfiguration
{
    NativeSocket socket;
    int family;
};

// Returns the most recent Winsock error code for structured error reporting.
[[nodiscard]] int last_socket_error() noexcept
{
    return WSAGetLastError();
}

// Closes a valid Windows socket and deliberately ignores errors during cleanup.
void close_socket(const NativeSocket socket) noexcept
{
    if (socket != invalid_socket)
    {
        static_cast<void>(closesocket(socket));
    }
}

// Winsock cancellation is not part of the current listener contract.
[[nodiscard]] bool should_retry_accept(const int error_code) noexcept
{
    static_cast<void>(error_code);
    return false;
}

class WinsockRuntime final
{
  public:
    // Starts the process-wide Winsock runtime and records the result for callers.
    WinsockRuntime() noexcept
    {
        WSADATA data{};
        result_ = WSAStartup(MAKEWORD(2, 2), &data);
    }

    // Balances a successful WSAStartup when the process-local helper is destroyed.
    ~WinsockRuntime()
    {
        // WSAStartup and WSACleanup must remain balanced.
        if (result_ == 0)
        {
            static_cast<void>(WSACleanup());
        }
    }

    WinsockRuntime(const WinsockRuntime &) = delete;
    WinsockRuntime &operator=(const WinsockRuntime &) = delete;

    // Exposes the WSAStartup code without exposing the WSADATA implementation detail.
    [[nodiscard]] int result() const noexcept
    {
        return result_;
    }

  private:
    int result_{};
};

// Ensures Winsock is initialized once and converts initialization failure to NetworkError.
[[nodiscard]] Result<void, NetworkError> ensure_socket_runtime()
{
    // Function-local static initialization is thread-safe since C++11 and runs once.
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

// Applies address-reuse and IPv6 behavior required by SpareNode's binding policy.
[[nodiscard]] bool configure_socket_security(const SocketConfiguration configuration)
{
    // Prevent another Windows socket from binding the same address and port.
    const BOOL exclusive = TRUE;
    if (setsockopt(configuration.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive),
                   static_cast<int>(sizeof(exclusive))) != 0)
    {
        return false;
    }

    if (configuration.family == AF_INET6)
    {
        // Avoid an implicit dual-stack listener; every exposed interface must be explicit.
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
#else
// POSIX sockets are ordinary integer file descriptors.
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket invalid_socket = -1;

struct SocketConfiguration
{
    NativeSocket socket;
    int family;
};

// Returns errno immediately after a failed POSIX socket operation.
[[nodiscard]] int last_socket_error() noexcept
{
    return errno;
}

// Closes a valid POSIX file descriptor and ignores errors during destructor cleanup.
void close_socket(const NativeSocket socket) noexcept
{
    if (socket != invalid_socket)
    {
        static_cast<void>(::close(socket));
    }
}

// A signal can interrupt POSIX accept without representing a socket failure.
[[nodiscard]] bool should_retry_accept(const int error_code) noexcept
{
    return error_code == EINTR;
}

// Keeps the cross-platform call site uniform; POSIX needs no socket runtime startup.
[[nodiscard]] Result<void, NetworkError> ensure_socket_runtime()
{
    // POSIX sockets need no process-wide initialization.
    return {};
}

// Restricts IPv6 sockets to IPv6 so listening interfaces remain explicit.
[[nodiscard]] bool configure_socket_security(const SocketConfiguration configuration)
{
    // Allow a restarted server to bind while connections from its previous run
    // remain in TIME_WAIT. Unlike Windows SO_REUSEADDR, this does not allow a
    // second live TCP listener to take over the same address and port.
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
    // Match Windows behavior by avoiding an implicit IPv4-mapped listener.
    return setsockopt(configuration.socket, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only,
                      sizeof(ipv6_only)) == 0;
}
#endif

class AddressInfo final
{
  public:
    // Takes ownership of the linked list returned by getaddrinfo.
    explicit AddressInfo(addrinfo *value) noexcept : value_(value)
    {
    }

    // Releases the complete getaddrinfo list, including all candidate addresses.
    ~AddressInfo()
    {
        // getaddrinfo allocates the linked list and requires freeaddrinfo for cleanup.
        if (value_ != nullptr)
        {
            freeaddrinfo(value_);
        }
    }

    AddressInfo(const AddressInfo &) = delete;
    AddressInfo &operator=(const AddressInfo &) = delete;

    // Returns the first candidate without transferring ownership.
    [[nodiscard]] addrinfo *get() const noexcept
    {
        return value_;
    }

  private:
    addrinfo *value_{};
};

// Converts a native IPv4/IPv6 sockaddr into SpareNode's platform-independent endpoint.
[[nodiscard]] Result<TcpEndpoint, NetworkError>
endpoint_from_address(const sockaddr *address, const NetworkOperation operation)
{
    // sockaddr_storage can contain either family; select the matching layout first.
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const void *raw_address = nullptr;
    std::uint16_t port = 0;

    if (address->sa_family == AF_INET)
    {
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
        raw_address = &ipv4->sin_addr;
        port = ntohs(ipv4->sin_port); // Convert network byte order to host byte order.
    }
    else if (address->sa_family == AF_INET6)
    {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(address);
        raw_address = &ipv6->sin6_addr;
        port = ntohs(ipv6->sin6_port);
    }
    else
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::state, 1});
    }

    if (inet_ntop(address->sa_family, raw_address, buffer.data(), buffer.size()) == nullptr)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    return TcpEndpoint{buffer.data(), port};
}

} // namespace

struct TcpConnection::Impl
{
    // Takes ownership of an accepted socket and stores its already-resolved peer endpoint.
    Impl(const NativeSocket socket, TcpEndpoint endpoint)
        : socket(socket), peer_endpoint(std::move(endpoint))
    {
    }

    // Closes the accepted socket when the connection owner goes out of scope.
    ~Impl()
    {
        // Impl is the single owner of the accepted native socket.
        close_socket(socket);
    }

    NativeSocket socket{invalid_socket};
    TcpEndpoint peer_endpoint;
};

struct TcpListener::Impl
{
    // Takes ownership of a socket that has already completed bind and listen.
    explicit Impl(const NativeSocket socket) noexcept : socket(socket)
    {
    }

    // Stops listening and releases the native socket when ownership ends.
    ~Impl()
    {
        // Impl is the single owner of the listening native socket.
        close_socket(socket);
    }

    NativeSocket socket{invalid_socket};
};

// Receives the implementation created by TcpListener::accept.
TcpConnection::TcpConnection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

// unique_ptr destroys Impl, whose destructor closes the native socket.
TcpConnection::~TcpConnection() = default;
// Moving unique_ptr transfers ownership and automatically empties the source object.
TcpConnection::TcpConnection(TcpConnection &&) noexcept = default;
TcpConnection &TcpConnection::operator=(TcpConnection &&) noexcept = default;

// A moved-from connection has no Impl and therefore owns no open socket.
bool TcpConnection::is_open() const noexcept
{
    return impl_ != nullptr && impl_->socket != invalid_socket;
}

// Returns a copy of the remote endpoint while keeping native details private.
std::optional<TcpEndpoint> TcpConnection::peer_endpoint() const
{
    if (!is_open())
    {
        return std::nullopt;
    }

    return impl_->peer_endpoint;
}

// Receives the implementation after bind and listen have both succeeded.
TcpListener::TcpListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

// unique_ptr destroys Impl, whose destructor closes the listening socket.
TcpListener::~TcpListener() = default;
// The same unique_ptr ownership rule applies to listening sockets.
TcpListener::TcpListener(TcpListener &&) noexcept = default;
TcpListener &TcpListener::operator=(TcpListener &&) noexcept = default;

// Resolves an explicit numeric address, creates a socket, binds it, and starts listening.
Result<TcpListener, NetworkError> TcpListener::bind(const TcpEndpoint &endpoint, const int backlog)
{
    // Reject configuration errors before creating any operating-system resource.
    if (endpoint.address.empty() || backlog <= 0)
    {
        return unexpected(NetworkError{NetworkOperation::bind, NetworkErrorDomain::validation, 1});
    }

    if (auto runtime = ensure_socket_runtime(); !runtime)
    {
        return unexpected(runtime.error());
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    // Numeric-only resolution prevents DNS lookup and accidental implicit interfaces.
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

    const AddressInfo addresses(resolved_addresses);
    NetworkError last_error{
        NetworkOperation::create_socket,
        NetworkErrorDomain::socket,
        0,
    };

    // getaddrinfo can return multiple candidates. Try each until one can listen.
    for (auto *current = addresses.get(); current != nullptr; current = current->ai_next)
    {
        const NativeSocket socket_handle =
            ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socket_handle == invalid_socket)
        {
            last_error = NetworkError{
                NetworkOperation::create_socket,
                NetworkErrorDomain::socket,
                last_socket_error(),
            };
            continue;
        }

        if (!configure_socket_security({socket_handle, current->ai_family}))
        {
            last_error = NetworkError{
                NetworkOperation::configure_socket,
                NetworkErrorDomain::socket,
                last_socket_error(),
            };
            close_socket(socket_handle); // Ownership has not reached Impl yet.
            continue;
        }

        if (::bind(socket_handle, current->ai_addr,
                   static_cast<SocketLength>(current->ai_addrlen)) != 0)
        {
            last_error = NetworkError{
                NetworkOperation::bind,
                NetworkErrorDomain::socket,
                last_socket_error(),
            };
            close_socket(socket_handle);
            continue;
        }

        if (::listen(socket_handle, backlog) != 0)
        {
            last_error = NetworkError{
                NetworkOperation::listen,
                NetworkErrorDomain::socket,
                last_socket_error(),
            };
            close_socket(socket_handle);
            continue;
        }

        // From this point Impl owns the socket and closes it automatically.
        return TcpListener(std::make_unique<Impl>(socket_handle));
    }

    return unexpected(last_error);
}

// Waits for one client and transfers the newly accepted socket to TcpConnection.
Result<TcpConnection, NetworkError> TcpListener::accept()
{
    if (!is_open())
    {
        return unexpected(NetworkError{NetworkOperation::accept, NetworkErrorDomain::state, 1});
    }

    sockaddr_storage peer_address{};
    SocketLength peer_address_length{};
    // accept blocks until a client arrives or the operating system reports an error.
    NativeSocket accepted_socket = invalid_socket;
    while (accepted_socket == invalid_socket)
    {
        peer_address_length = static_cast<SocketLength>(sizeof(peer_address));
        accepted_socket = ::accept(impl_->socket, reinterpret_cast<sockaddr *>(&peer_address),
                                   &peer_address_length);
        if (accepted_socket == invalid_socket)
        {
            const int error_code = last_socket_error();
            if (!should_retry_accept(error_code))
            {
                return unexpected(NetworkError{
                    NetworkOperation::accept,
                    NetworkErrorDomain::socket,
                    error_code,
                });
            }
        }
    }

    auto endpoint = endpoint_from_address(reinterpret_cast<const sockaddr *>(&peer_address),
                                          NetworkOperation::query_peer_endpoint);
    if (!endpoint)
    {
        // No TcpConnection owns the accepted socket yet, so clean it up manually.
        close_socket(accepted_socket);
        return unexpected(endpoint.error());
    }

    // Successful construction transfers ownership to TcpConnection::Impl.
    return TcpConnection(
        std::make_unique<TcpConnection::Impl>(accepted_socket, std::move(endpoint.value())));
}

// Queries the effective local address, including a port selected after binding to port zero.
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
    SocketLength local_address_length = sizeof(local_address);
    // getsockname is required to discover a system-selected port after binding to zero.
    if (getsockname(impl_->socket, reinterpret_cast<sockaddr *>(&local_address),
                    &local_address_length) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::query_local_endpoint,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    return endpoint_from_address(reinterpret_cast<const sockaddr *>(&local_address),
                                 NetworkOperation::query_local_endpoint);
}

// Reports whether this listener still owns a usable native socket.
bool TcpListener::is_open() const noexcept
{
    return impl_ != nullptr && impl_->socket != invalid_socket;
}

} // namespace sparenode::network
