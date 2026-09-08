# HTTP request parser

SpareNode implements a deliberately narrow HTTP/1.1 request subset. The parser
accepts `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, and `OPTIONS`, requires one
syntactically valid `Host` header, and supports either one decimal `Content-Length`
or a single case-insensitive `Transfer-Encoding: chunked` field. TE/CL combinations,
duplicate TE fields, coding lists, and coding parameters are rejected as ambiguous
framing (400). A single unsupported coding such as `gzip` produces 501. Fields
are resolved before body ingestion; there is no fallback to a different length.

`parse_http_request()` is stateless. Its input is a caller-owned bounded byte
buffer. Incomplete input returns a successful result with `complete == false`,
so the caller can append bytes and parse again. A complete result contains
metadata views into that same buffer and reports the consumed wire byte count;
trailing bytes are not consumed and may begin a pipelined request. Fixed-length
bodies borrow the input; chunked bodies retain shared immutable decoded storage
that survives copying the request view. The caller must still keep the metadata
buffer alive, unmoved, and unmodified while using the request views.

The parser enforces independent limits for the request line, total header bytes,
header count, and declared body bytes. Defaults are conservative and callers can
supply `HttpRequestParserLimits` appropriate to a configured server. Limit and
syntax failures are represented by `HttpRequestParseErrorCode` with the source
byte offset. Declared sizes are checked before payload copying; both metadata and
decoded storage remain bounded even when later framing is malformed. The
stateless convenience parser may allocate up to the decoded-body limit for chunked
payloads. Sessions use the streaming interface below rather than reparsing bodies.

## Incremental body ingestion

`parse_http_request_head()` validates the request line, headers, and framing before
the body arrives. `HttpBodyDecoder` then accepts fresh wire fragments and a mutable
output span. Its result reports consumed wire bytes, produced payload bytes, and
completion. The caller retains unconsumed input and can immediately send produced
bytes to a bounded buffer or future temporary-file sink. An empty/full output span
applies backpressure without losing input. Neither mode owns payload storage or
performs socket I/O; fixed-length and chunked payloads use this same interface.

Call `finish()` on EOF. An unfinished fixed-length body, size line, chunk payload,
CRLF, or trailer section returns `incomplete_body`. Syntax, limits, and EOF errors
are terminal. Callers must discard the entire failed request, including any bytes
already written to a temporary sink. No endpoint is invoked for incomplete input.

The chunk decoder follows [RFC 9112 section 7.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1).
Size arithmetic checks overflow and the cumulative decoded payload limit before
copying data. CRLF may span receives; bare LF, signed/invalid hex sizes, and malformed
payload terminators fail closed. Only the final empty trailer line completes a
chunked body, and the decoder leaves subsequent request bytes untouched.

Chunk extensions are syntax-checked and ignored, including token or quoted values,
quoted pairs, and whitespace at extension separators. Unknown extension names do
not change decoding. Default limits are 1,024 bytes per size/extension line
(excluding CRLF) and 65,536 cumulative framing bytes (including chunk delimiters and
trailers). Many tiny chunks cannot evade this cumulative overhead limit.

Trailers are validated then discarded; they never overwrite or supplement request
headers. Defaults permit 8,192 trailer bytes including the final CRLF and 32 fields.
Framing, routing, authentication, connection, and content-processing fields are
rejected: `Content-Length`, `Transfer-Encoding`, `Host`, `Connection`, `Trailer`,
`TE`, `Authorization`, `Proxy-Authorization`, `Cookie`, `Expect`, `Upgrade`,
`Content-Encoding`, `Content-Type`, and `Content-Range`. Other syntactically valid
trailers are ignored, so they cannot affect endpoint authorization or processing.
Limits are C++ parser settings; payload defaults remain 1 MiB. Payload or framing
limit errors map to 413, while chunk/trailer syntax errors map to 400.

SN-087 can consume the decoder's output spans directly for either framing mode.
The current `BufferedRequest` adapter retains only bounded metadata and decoded
body storage for the existing route API; it does not implement file ingestion.

The required `Host` field uses a deliberately strict, allocation-free ASCII
authority subset. It accepts DNS-style names of at most 253 bytes with labels
of at most 63 bytes and an optional terminal root dot, strict four-octet
dotted-decimal IPv4 addresses, and bracketed IPv6 literals. IPv6 literals may
use zero compression and may end with an embedded IPv4 address. An optional
decimal port must be between 0 and 65535, including after a terminal root dot.
Whitespace, underscores, malformed labels or literals, unbracketed IPv6, IPv6
zone identifiers, empty ports, and out-of-range numeric components are rejected
as `invalid_host` before routing.

Only origin-form targets beginning with `/` are accepted. Raw spaces, ASCII
control bytes, fragments, malformed percent escapes, backslashes, bytes outside
the RFC 3986 path/query grammar, obsolete folded headers, duplicate `Host`,
duplicate `Content-Length`, bare line feeds, and versions other than HTTP/1.1 are rejected.
Filesystem decoding and containment remain the responsibility of `SafePath` at
the later routing/filesystem boundary.
