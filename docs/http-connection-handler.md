# HTTP connection handler

`handle_http_connection()` is the HTTP/TCP composition boundary used by dispatcher
workers. It incrementally reads one HTTP/1.1 request into caller-independent,
bounded storage, invokes `parse_http_request()` after each receive, dispatches a
complete borrowed request through `HttpRouter`, and streams the resulting
`HttpResponse` through `write_http_response()`.

The default request boundary is derived from the independent parser limits:
request-line bytes, the line terminator, header bytes, and body bytes. Native
receives request at most `receive_chunk_bytes` at a time. Invalid zero limits and
overflowing combined limits are rejected before network I/O. The current parser
requires a complete bounded body in memory; SN-087 will replace large-body
retention with temporary-file ingestion.

## Connection policy

Version 0.1 handles exactly one request and one response per TCP connection. If
the initial receive contains pipelined bytes, the parser reports the first exact
request boundary and the session keeps all input storage alive while routing and
writing that response. Bytes after the boundary are never interpreted as part of
the first request. They are discarded only when the session returns and its
exclusive `TcpConnection` closes. Persistent connections can be added later
without changing parser boundaries.

Malformed requests receive an empty bounded error response when transmission is
still possible. The mapping uses 400 for general syntax, 413 for body size, 414
for request-target size, 431 for header limits, 501 for unsupported transport
features, and 505 for unsupported HTTP versions. Route failures receive 500.
Network receive/send failures remain structured `NetworkError` values.

## Cancellation and deadlines

Every receive always carries the dispatcher worker's stop token. An optional
`HttpRequestDeadlineProvider` may add an absolute steady-clock deadline for the
next read but cannot replace cancellation. It receives the current read phase
(`headers` or `body`) and the session start time. This is the injection boundary
for SN-089 to combine header inactivity, body inactivity, and total-request
budgets without placing HTTP policy in the socket layer.

There are no session timer threads and no active polling loops. Deadlines are
enforced by the deadline-aware native socket wait introduced by SN-094.
