# TCP listener

The public TCP API is declared under `include/sparenode/network`. It provides
move-only `TcpListener` and `TcpConnection` types so native socket ownership is
released automatically on every return path.

```cpp
#include <stop_token>
#include <utility>

#include "sparenode/network/tcp_listener.hpp"

void accept_one()
{
    auto listener_result =
        sparenode::network::TcpListener::bind({"127.0.0.1", 8080});
    if (!listener_result)
    {
        const auto &error = listener_result.error();
        // Inspect error.operation, error.domain, and error.code.
        return;
    }

    auto listener = std::move(listener_result.value());
    std::stop_source stop_source;
    auto connection_result = listener.accept(stop_source.get_token());
}
```

`bind` accepts numeric IPv4 and IPv6 addresses only. An empty address and host
names such as `localhost` are rejected, preventing an implicit wildcard bind.
Use `0.0.0.0` or `::` only when exposure on every matching interface is
intentional. Port `0` requests an available system-selected port; call
`local_endpoint()` to obtain the effective address and port. The default backlog
is `128` pending connections and can be overridden with the second `bind`
argument.

`accept()` blocks indefinitely until a client connects or the operating system
reports an error. The `accept(std::stop_token)` overload adds cooperative
cancellation. A stop request wakes an accept operation that is already blocked;
it does not wait for another client to connect. Cancellation returns an error
whose operation is `accept`, whose domain is `cancellation`, and whose code is
zero, so callers do not need to interpret platform-specific socket codes.

Cancellation does not close the listener. After the cancelled call has returned,
the same listener can accept another connection. A stop requested before the call
cancels it immediately, and a token remains stopped for subsequent calls. If a
stop request races with connection arrival, either the connection completes
before the request is observed or cancellation wins and any connection accepted
during that cancellation window is released.

`TcpListener` is not thread-safe: never run more than one accept operation at a
time, and do not invoke other listener methods, move it, or destroy it until the
accept operation has returned. The supported shutdown order is:

1. Request stop through the `std::stop_source` owned by the server.
2. Wait for the thread or task running `accept(stop_token)` to finish.
3. Destroy the listener only after that operation has returned.

Internally, cancellation uses a private loopback wake channel monitored together
with the listening socket. The channel is created lazily by the first cancellable
accept and reused by later sequential accepts. Cancellation does not rely on
unsafely closing the listener from another thread and does not use periodic
polling or a busy-wait loop. The listener socket, poller, and wake channel travel
through the internal wait layer as one non-owning context.

Operations return `sparenode::Result<Value, NetworkError>`. Errors identify the
failed operation, error domain, and native numeric code without embedding paths,
host names, or other potentially sensitive diagnostic text. The native code is
platform-specific and should be used for diagnostics rather than application
control flow.

Moving a listener or connection transfers ownership and leaves the source
closed. Destroying the owning object closes its native socket. The public
headers never expose Windows `SOCKET` values or Linux file descriptors.
