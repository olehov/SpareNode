# HTTP response and streaming layer

SpareNode represents outgoing HTTP/1.1 messages with the move-only
`http::HttpResponse` type. Construction validates the status line and every
application-provided header before network transmission begins. Response values
own their strings so request-buffer lifetimes cannot affect outgoing metadata.

Construction uses the strongly typed `http::HttpStatusCode` enumeration. Callers
select names such as `ok`, `not_found`, or `internal_server_error`; only the
serializer converts the selected status to its numeric wire representation.

## Framing

The transport generates `Content-Length` from the selected body representation.
Callers cannot supply `Content-Length` or `Transfer-Encoding`, which prevents
conflicting framing metadata. Informational responses, `204`, and `304` omit
`Content-Length`; statuses that cannot carry a message body reject a non-empty
body during construction.

Header names use the HTTP token grammar. Reason phrases and field values reject
control bytes that could inject a new protocol line. The serialized head has a
32 KiB boundary and at most 100 application fields.

The transport also owns connection policy: every final (non-1xx) response includes
exactly one `Connection: close`. Application-provided `Connection` and `Keep-Alive`
fields are rejected case-insensitively during construction with
`managed_connection_header`. Handlers omit these fields. Generated fields count
toward the serialized head boundary. Informational serialization does not add close;
the current session requires a final response and converts a standalone 1xx handler
result to 500 because endpoint-driven interim exchanges and upgrades are not implemented.
The session itself may send 100 Continue before receiving an expected body.

The transport generates `Date` for every final response (including 5xx) in UTC
IMF-fixdate form, for example `Sun, 06 Nov 1994 08:49:37 GMT`. It samples the system
clock at response construction and stores that value, so serialization is stable
and independent of process locale or concurrent calls. Application-provided Date
is rejected case-insensitively with `managed_date_header`; the generated 37-byte
field counts toward the existing head limit. Informational responses omit Date.
This satisfies [RFC 9110 section 6.6.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-6.6.1)
for this origin server with a system clock.

## Body representations

Small bodies are owned directly by the response and are limited to 1 MiB. This
path is suitable for diagnostics, metadata, and API responses.

Streaming responses declare their exact byte length and own a C++23
`std::move_only_function` reader. The writer repeatedly gives that reader a
16 KiB destination and immediately sends each produced chunk. Consequently,
body size does not determine transport memory use. A successful zero-byte read
before the declared length is treated as an early end rather than silently
producing a truncated response.

The body reader may retain a file, generator, or other move-only cursor. It must
not report more bytes than the supplied destination can hold. Reader errors and
exceptions are contained as structured `HttpResponseWriteError` values.

## Network behavior

`write_http_response()` retries partial TCP sends until each supplied span is
complete. The same stop token is passed to the TCP connection and streaming body
reader, so application shutdown can cancel either boundary. Network failures,
body-source failures, invalid source results, early end-of-body, and allocation
failures remain distinguishable.

One streaming response is a one-shot value: transmitting it advances its owned
reader. A failed or completed streaming response must not be transmitted again.
Request deadlines and routing belong to the HTTP session layer. It passes the
original request method to `write_http_response()`. For HEAD, the writer sends the
same serialized response head, including the selected representation's Content-Length,
and returns before memory-body transmission or streaming-buffer allocation/reader
invocation. Existing bodyless-status rules still omit Content-Length for 204/304.
The default writer method is GET for callers that do not supply request context.

For large files, construct a known-length streaming response using representation
metadata and a lazy body reader. Open the transfer handle inside that reader when
possible; HEAD never calls it. Response construction and authorization still run.
The transport cannot prevent eager file I/O or other work inside an endpoint handler.
Explicit HEAD handlers must describe the GET representation, not substitute a zero
length simply because HEAD sends no content.

These semantics follow [RFC 9110 section 9.3.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.3.2).
