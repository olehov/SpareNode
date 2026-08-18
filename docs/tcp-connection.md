# TCP connection I/O

`TcpConnection` owns one accepted TCP socket and exposes byte-oriented,
caller-buffered `receive` and `send` operations. The API does not allocate memory
based on peer input and does not expose a native Windows `SOCKET` or Linux file
descriptor.

```cpp
#include <array>
#include <cstddef>
#include <stop_token>

#include "sparenode/network/tcp_connection.hpp"

void read_once(sparenode::network::TcpConnection &connection,
               const std::stop_token stop_token)
{
    std::array<std::byte, 4096> buffer{};
    const auto result = connection.receive(buffer, stop_token);
    if (!result)
    {
        const auto &error = result.error();
        // Inspect error.operation, error.domain, and error.code.
        return;
    }

    if (result.value() == 0)
    {
        // The peer performed an orderly shutdown.
        return;
    }

    // Process exactly result.value() bytes from buffer.
}
```

Both operations transfer at most one span and may complete partially. A caller
that needs to send an entire logical message must call `send` again with the
unsent suffix. `receive` returns zero bytes only when the peer has performed an
orderly shutdown. Empty input buffers are rejected because a zero-length receive
would otherwise be indistinguishable from peer shutdown.

The overloads without a stop token wait indefinitely without creating a wake
channel. The `std::stop_token` overloads add cooperative cancellation. A stop
request made while the operation is waiting wakes it without closing the TCP
connection. Cancellation returns a structured error whose operation is
`receive` or `send`, whose domain is `cancellation`, and whose code is zero.

Cancellation cannot undo bytes already transferred. If readiness is observed
and a native operation completes before the stop request is observed, the byte
count is returned and the transfer wins that race. The caller can check its stop
token before starting another operation.

Internally, accepted sockets are nonblocking. SpareNode waits with `WSAPoll` on
Windows or `poll` on Linux and uses a private authenticated loopback wake channel
for stoppable waits. The channel is created lazily on the first cancellable
operation and reused by later sequential operations on the same connection.
`ConnectionIo` groups the socket, poller, wake channel, and native transfer
operations behind one internal coordinator, so those stable dependencies are not
passed separately through each call layer. There is no periodic timeout or
busy-wait loop. POSIX sends suppress `SIGPIPE`, allowing peer disconnection to be
returned as a structured socket error instead of terminating the process.

`TcpConnection` is move-only and not thread-safe. Do not call `receive`, `send`,
`peer_endpoint`, or other methods concurrently on the same object. Do not move
or destroy a connection until its active operation has returned. The supported
shutdown order is:

1. Request cancellation through the server-owned stop source.
2. Wait for the active connection operation and handler to return.
3. Destroy the connection, which closes its socket through RAII.

Moving a connection transfers ownership and leaves the source closed. Operations
on a moved-from connection return a `state` error. Native error codes remain
platform-specific and are intended for diagnostics rather than application
control flow.
