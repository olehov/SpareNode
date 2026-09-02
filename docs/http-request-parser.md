# HTTP request parser

SpareNode implements a deliberately narrow HTTP/1.1 request subset. The parser
accepts `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, and `OPTIONS`, requires one
syntactically valid `Host` header, supports one decimal `Content-Length`, and rejects
`Transfer-Encoding`. Rejecting ambiguous length mechanisms prevents request
smuggling between future protocol layers.

`parse_http_request()` is stateless. Its input is a caller-owned bounded byte
buffer. Incomplete input returns a successful result with `complete == false`,
so the caller can append bytes and parse again. A complete result contains
views into that same buffer and reports `consumed_bytes`; trailing bytes are not
consumed and may begin a pipelined request. The caller must keep the buffer
alive, unmoved, and unmodified while using the request views.

The parser enforces independent limits for the request line, total header bytes,
header count, and declared body bytes. Defaults are conservative and callers can
supply `HttpRequestParserLimits` appropriate to a configured server. Limit and
syntax failures are represented by `HttpRequestParseErrorCode` with the source
byte offset. Malformed input never causes body-sized allocation: headers are the
only parser-owned collection and their count and source bytes are bounded.

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
