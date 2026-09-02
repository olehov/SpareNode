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
Connection persistence, request deadlines, routing, and HEAD-specific body
suppression belong to the HTTP session layer built on top of this component.
