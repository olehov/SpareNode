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

Every receive carries the dispatcher worker's stop token and an absolute
steady-clock deadline. `HttpRequestTimeouts` groups header inactivity (10 seconds),
body inactivity (10 seconds), and total receive time (30 seconds). Runtime server
settings carry these budgets from the validated `header_timeout_ms`,
`body_timeout_ms`, and `request_timeout_ms` directives into the application handler.

Inactivity starts at session entry and renews only after a nonempty receive. The
body phase begins once the complete header terminator arrives; its inactivity
anchor is the timestamp of that receive. Parsing and buffer preparation do not
renew the timer. Each read uses the earlier of its phase deadline and the immutable
total deadline. A shorter total budget is valid and prevents trickle traffic from
extending a session indefinitely.

An optional `HttpRequestDeadlineProvider` receives the phase and session start;
its result can only shorten the configured deadline. Returning no value leaves
the built-in policy in effect. Provider exceptions remain contained at the session
boundary. Direct C++ configurations reject nonpositive or unrepresentable budgets
before I/O; persistent configuration additionally caps each value at 24 hours.

Expiry returns `NetworkErrorDomain::timeout` with the receive operation, closes
the owned connection, and releases the dispatcher worker. The configured failure
observer logs the structured error without request contents. No HTTP error response
is attempted after a receive timeout. Cancellation remains a distinct failure and
wins if already observable at the same wait boundary as timeout.

These are request-receive deadlines. They start when the worker begins the session,
not at TCP accept, and do not interrupt route execution or response writes. Native
readiness already reported by the poller may complete a receive at the deadline
boundary, consistent with the socket I/O contract.

There are no session timer threads and no active polling loops. Deadlines are
enforced by the deadline-aware native socket wait introduced by SN-094.

Deadline arithmetic and renewal are tested using explicit timestamps, including
exact representable boundaries. Loopback tests cover idle clients, partial headers,
partial bodies, total expiry despite repeated receive progress, provider clamping,
cancellation, logging, and worker reuse. A timeout closes the connection; unread
input may make the peer observe a TCP reset instead of an orderly EOF.
