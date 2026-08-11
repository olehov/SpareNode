# TCP listener

The public TCP API is declared under `include/sparenode/network`. It provides
move-only `TcpListener` and `TcpConnection` types so native socket ownership is
released automatically on every return path.

```cpp
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
    auto connection_result = listener.accept(); // Blocks until a client connects.
}
```

`bind` accepts numeric IPv4 and IPv6 addresses only. An empty address and host
names such as `localhost` are rejected, preventing an implicit wildcard bind.
Use `0.0.0.0` or `::` only when exposure on every matching interface is
intentional. Port `0` requests an available system-selected port; call
`local_endpoint()` to obtain the effective address and port.

Operations return `sparenode::Result<Value, NetworkError>`. Errors identify the
failed operation, error domain, and native numeric code without embedding paths,
host names, or other potentially sensitive diagnostic text. The native code is
platform-specific and should be used for diagnostics rather than application
control flow.

Moving a listener or connection transfers ownership and leaves the source
closed. Destroying the owning object closes its native socket. The public
headers never expose Windows `SOCKET` values or Linux file descriptors.
