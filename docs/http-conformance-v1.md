# HTTP/1.1 conformance profile v1

Profile revision: **v1-draft.4**, 2026-09-09.
Implementation baseline: SN-099 implementation based on `5003272`.
Tracking: [SN-096](https://github.com/olehov/SpareNode/issues/106), within
[SN-095](https://github.com/olehov/SpareNode/issues/103) and
[Sprint 3](https://github.com/olehov/SpareNode/issues/105).

This is a draft compatibility contract for SpareNode's restricted HTTP/1.1
origin-server transport. It records current behavior separately from planned
changes. It does not assert full HTTP/1.1 conformance. SN-096 remains incomplete
until SN-100 delivers its behavior and this profile is reconciled
with their implementation and tests.

## Classification

- **Implemented**: the stated behavior exists at the baseline.
- **Intentionally restricted**: a deliberate local policy or supported subset;
  this label does not waive an applicable RFC requirement.
- **Deferred**: behavior is missing or still requires a compatibility decision.
  Endpoints must not depend on it yet.
- **Not applicable**: behavior belongs to a role or protocol outside this profile.

## Request compatibility

| Behavior | Classification | Current contract and follow-up | Reference |
| --- | --- | --- | --- |
| Request-line and field parsing | Implemented | Strict CRLF, token field names, bounded fields, and rejection of obsolete folding. | [RFC 9112 §2.2](https://www.rfc-editor.org/rfc/rfc9112.html#section-2.2), [§5](https://www.rfc-editor.org/rfc/rfc9112.html#section-5) |
| Origin-form targets | Implemented | Absolute paths beginning with `/`, with optional query; fragments and invalid escapes are rejected. | [RFC 9112 §3.2.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2.1) |
| Absolute-form and asterisk-form | Implemented | HTTP absolute targets normalize to origin-form path/query; URL authority replaces the downstream Host, including on mismatch. OPTIONS alone accepts `*`, returning server-wide 204 without invoking resource routes. | [RFC 9112 §3.2.2](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2.2), [§3.2.4](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2.4) |
| Absolute URI schemes | Intentionally restricted | Only case-insensitive `http://` is supported. HTTPS, other schemes, userinfo, fragments, and unsupported authority syntax fail 400. The parser guide specifies normalization and ownership. | [RFC 9110 §4.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-4.2) |
| Methods | Intentionally restricted | Parser accepts GET, HEAD, POST, PUT, DELETE, OPTIONS. Other valid method tokens produce 501; acceptance does not imply an installed endpoint. | [RFC 9110 §9.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.1) |
| Host | Intentionally restricted | Exactly one nonempty Host is required. The supported ASCII DNS/IPv4/bracketed-IPv6 authority grammar is described in the parser guide; it is not the complete URI reg-name grammar. | [RFC 9110 §7.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.2) |
| HTTP versions | Intentionally restricted | HTTP/1.1–1.9 use HTTP/1.1 semantics/responses. Well-formed unsupported versions, including 1.0, yield 505; malformed single-digit version grammar yields 400. | [RFC 9112 §2.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-2.3) |
| Fixed request length | Implemented | One decimal Content-Length; overflow, duplicates, and invalid values are rejected. No length field means an empty body. | [RFC 9112 §6.2](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.2), [§6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) |
| Chunked requests and trailers | Implemented | SN-097 provides incremental decoding, checked sizes, bounded ignored extensions and trailers, TE/CL rejection, and EOF validation. | [RFC 9112 §6.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.1), [§7.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1) |
| Transfer-coding subset | Intentionally restricted | One case-insensitive chunked coding only. Coding lists, parameters, and repeated TE fields fail with 400; other single coding tokens fail with 501. Trailer policy and limits are specified in the parser guide. | [RFC 9112 §6.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.1) |
| Expect / 100-continue | Deferred | The session waits for a complete request before routing and has no interim-response handshake. Clients must not rely on receiving 100 Continue. Requires follow-up review under SN-095. | [RFC 9110 §10.1.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-10.1.1) |

## Response and connection compatibility

| Behavior | Classification | Current contract and follow-up | Reference |
| --- | --- | --- | --- |
| Response framing | Implemented | Validated status and fields; generated Content-Length; application-supplied Content-Length and Transfer-Encoding are rejected. | [RFC 9110 §8.6](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.6) |
| Bodyless statuses | Implemented | Informational, 204, and 304 responses reject nonempty bodies and omit Content-Length. This does not imply a session-level interim-response exchange. | [RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) |
| HEAD | Deferred | Parsed and routed, but the session does not suppress handler content. SN-100 owns body suppression and representation metadata. | [RFC 9110 §9.3.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.3.2) |
| Known-length streaming | Implemented | Bounded writer buffer, partial-send retries, exact declared length, structured reader failures, and cooperative cancellation. | [RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) |
| Persistence | Intentionally restricted | One request is handled per connection.  pipelined requests are not executed. | [RFC 9112 §9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3) |
| Connection-close signaling and draining | Implemented | Transport-generated Connection: close for every final response; endpoint Connection/Keep-Alive fields rejected. Successful final writes use send half-close, then bounded and cancellable draining. Budget exhaustion ends normally; peer resets or excess input can still prevent delivery. | [RFC 9112 §9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6) |
| Date generation | Deferred | The response layer does not automatically generate Date. Applicability and generation need review under SN-095 before a final conformance claim. | [RFC 9110 §6.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.6.1) |
| Ranges, conditional requests, representation metadata | Deferred | No file-serving endpoint contract exists yet. File handlers must specify their supported semantics; generic routing does not implement them. | [RFC 9110 §8](https://www.rfc-editor.org/rfc/rfc9110.html#section-8), [§13](https://www.rfc-editor.org/rfc/rfc9110.html#section-13), [§14](https://www.rfc-editor.org/rfc/rfc9110.html#section-14) |
| Proxy forwarding and CONNECT tunnels | Not applicable | SpareNode has no proxy/gateway role or tunnel endpoint. | [RFC 9110 §7.6](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6), [§9.3.6](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.3.6) |

HTTP/2, HTTP/3, and TLS are outside this profile. Upgrade negotiation is deferred;
the current session has no protocol-switching path.

## Resource limits and lifetime

Defaults at the baseline:

| Boundary | Default |
| --- | --- |
| Request line, excluding CRLF | 8,192 bytes |
| Header section, including terminating CRLF | 32,768 bytes |
| Request header count | 100 |
| Request body | 1,048,576 bytes |
| Chunk size/extension line, excluding CRLF | 1,024 bytes |
| Cumulative chunk framing, including trailers | 65,536 bytes |
| Trailer bytes / field count | 8,192 bytes / 32 fields |
| Session receive chunk | 16,384 bytes |
| Header inactivity / body inactivity | 10 seconds / 10 seconds |
| Total request receive budget | 30 seconds |
| Serialized response head | 32,768 bytes |
| Application response fields | 100 |
| Owned response body | 1,048,576 bytes |
| Streaming writer buffer | 16,384 bytes |
| Post-response drain bytes / absolute time | 65,536 bytes / 100 ms |
| Drain buffer | 4,096 bytes |

Parser/storage settings are C++ configuration boundaries. Persistent server
configuration exposes `header_timeout_ms`, `body_timeout_ms`, and
`request_timeout_ms`, each accepting 1 through 86,400,000 milliseconds.
The session retains bounded request metadata and decoded payload plus receive and
decoder scratch buffers; it still buffers the complete decoded request body.
Chunk framing is processed once and discarded rather than accumulated with payload.
The response streaming limit is independent of the declared stream length.

Receive budgets start when a worker enters the session, not at accept. Nonempty
receives renew inactivity only. The earlier phase/total deadline bounds each
receive; an injected provider may only shorten it. Timeout and cancellation have
distinct portable errors. Timeout closes the connection without an HTTP response.
These budgets do not interrupt route execution or bound response transmission.

## Endpoint integration and verification

Request views borrow session storage and must not escape its lifetime. Filesystem
decoding and containment belong to the filesystem boundary. Syntactically valid
Host or request-target values do not authorize access to a configured share.
Streaming readers own their cursors and must honor cancellation and buffer bounds.
Endpoints must not implement their own competing message-framing policy.

Current behavior is exercised by `tests/http/http_request_parser_test.cpp`,
`http_connection_handler_test.cpp`, `http_response_test.cpp`, `http_router_test.cpp`,
`http_body_decoder_test.cpp`, and `request_deadlines_test.cpp`, plus configuration
and network tests. `request_target_test.cpp` and router/session tests cover
normalization, authoritative Host replacement, query storage lifetime, OPTIONS *,
wire length boundaries, and version syntax/status behavior. SN-097 coverage includes every split of a chunked fixture,
small output buffers, exact limits, malformed framing, premature EOF, trailer
isolation, and chunked loopback timeout/cancellation behavior.
SN-099 coverage includes generated close headers, exact response-head bounds,
unread socket input, half-close receive access, byte/time drain limits, and cancellation.
The corresponding implementation contracts are in the
[parser](http-request-parser.md), [response](http-response.md),
[router](http-router.md), and [session](http-connection-handler.md) guides.

To finalize v1, update each deferred SN-099–SN-100 row after its implementation,
link its regression coverage, record remaining restrictions, and resolve the
Expect/Date review items under SN-095. Verify Windows and Linux behavior and run
the documentation checks. Change the baseline and revision whenever the recorded
wire contract changes; do not silently describe planned behavior as implemented.
