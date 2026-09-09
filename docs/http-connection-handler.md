# HTTP connection handler

`handle_http_connection()` is the HTTP/TCP composition boundary used by dispatcher
workers. It incrementally reads one HTTP/1.1 request into caller-independent,
bounded storage, validates metadata with `parse_http_request_head()`, dispatches a
complete borrowed request through `HttpRouter`, and streams the resulting
`HttpResponse` through `write_http_response()`.

`BufferedRequest` freezes validated metadata storage and feeds fresh body bytes to
one persistent `HttpBodyDecoder` for fixed-length and chunked framing. Only decoded
payload is retained for routing. Chunk syntax is never reparsed on the next receive.
Native receives request at most `receive_chunk_bytes` at a time, bounded also by
the combined request-line, CRLF, header, and payload configuration. Zero receive
chunks and overflowing combined limits are rejected before I/O. Framing and trailer
limits apply independently of decoded payload limits. SN-087 can replace the
bounded body buffer with temporary-file ingestion through the same decoder API.

## Connection policy

Version 0.1 handles exactly one request and one response per TCP connection. If
the initial receive contains pipelined bytes, the parser reports the first exact
request boundary and the session keeps metadata and decoded storage alive while routing and
writing that response. Bytes after the boundary are never interpreted as part of
the first request. Already received trailing bytes are discarded; unread socket
bytes are drained after sending the final response. Persistent connections can be
added later without changing parser boundaries.

The response transport generates `Connection: close` for every final response,
including routed responses and parser/route errors. Endpoint-supplied `Connection`
and `Keep-Alive` headers fail response validation. A standalone informational route
result becomes 500; the session does not implement interim exchanges or upgrades.

After a successful write, `TcpConnection::shutdown_send()` sends FIN after queued
output while retaining the receive direction. The session then discards additional
socket input until peer EOF, `max_drain_bytes` (64 KiB by default), or the fixed
`drain_timeout` (100 ms by default). Progress never renews this deadline. The buffer
is fixed at 4 KiB, and each read is capped by the remaining byte budget. Already
buffered trailing request bytes need no additional socket reads and are covered by
the existing receive-buffer bound. Extra requests are never dispatched.

Direct C++ configuration requires a nonzero byte limit and a positive drain timeout
of at most 24 hours. These settings are not yet persistent configuration directives.
Budget exhaustion after a final response is normal completion, not a request timeout.
Cancellation and native shutdown/read errors remain structured failures for existing
logging. Failed response writes and request receive timeouts retain their immediate
termination behavior and do not start a new drain phase.

This staged close follows [RFC 9112 section 9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6).
It preserves responses in the tested unread/pipelined-input scenarios, but cannot
guarantee delivery if the peer resets or continues sending beyond the finite budgets.

Malformed requests receive an empty bounded error response when transmission is
still possible. EOF after a partial request also receives 400; an idle peer EOF
remains a successful close. The mapping uses 400 for general syntax and framing
ambiguity, 413 for body or chunk/trailer metadata limits, 414
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
