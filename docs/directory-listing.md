# Filesystem read API

SpareNode exposes each configured filesystem location through two read-only HTTP routes.
For a location configured as `/api/Documents`:

- `GET /api/Documents` lists the location root.
- `GET /api/Documents/*` lists a directory or serves a regular file identified by the
  wildcard suffix.

The suffix is an untrusted, URL-encoded UTF-8 relative path. It passes through
`SafePath` before any directory operation. A location with `read = false` returns
`403 Forbidden` without inspecting the directory.

## Directory response

A successful request returns `application/json; charset=utf-8` and a stable,
name-sorted array:

```json
{
  "entries": [
    {
      "name": "report.txt",
      "type": "file",
      "size": 1536,
      "modifiedAt": "2026-09-23T12:30:00Z"
    },
    {
      "name": "archive",
      "type": "directory",
      "size": null,
      "modifiedAt": "2026-09-22T09:10:11Z"
    }
  ]
}
```

`type` is `file`, `directory`, `symlink`, or `other`. Only regular files expose
a byte size. Modification times use second-resolution RFC 3339 UTC. Responses
contain basenames and entry metadata; configured API paths and native host paths are never
included.

## File response

A regular file returns `200 OK`, `Content-Disposition: inline`,
`X-Content-Type-Options: nosniff`, and the exact `Content-Length` captured from its opened native
handle. SpareNode selects `Content-Type` from the immutable MIME registry loaded during startup.
The lookup uses the decoded filename extension and is ASCII case-insensitive. Unknown types use
`application/octet-stream`.

HTML, SVG, and JavaScript-family files use `text/plain; charset=utf-8` regardless of their
configured mapping. Rendering active content is a client concern, and serving it as executable
same-origin content would allow a shared file to act as part of the SpareNode application. The
`nosniff` response field prevents clients from overriding this policy through content inspection.

The response owns the opened file handle and reads it incrementally through the HTTP writer's fixed
16 KiB buffer. File contents are therefore not copied into an application-sized memory allocation
before transmission.

`HEAD` uses the same route and reports the file headers without reading its payload. A cancelled
request is observed before the next native read. Closing or destroying the response closes the
file handle, including after a client disconnect or read failure.

## Safety and limits

Each child passes through `SafePath` independently. Entries that resolve outside
the configured root, cross an unsupported Windows reparse point, or cannot be
represented as a safe public path are omitted. This keeps one unsafe host entry
from making the rest of a directory unavailable and avoids disclosing its
metadata.

File downloads also pass through `SafePath` before opening. After opening, SpareNode resolves the
stable native handle and checks it against a separately opened shared-root handle. This second
check prevents a filesystem replacement between path validation and open from redirecting a
download outside the configured root. Subsequent reads stay bound to that opened object even if
its pathname is renamed or replaced.

One request inspects at most 512 host entries. A larger directory returns
`413 Content Too Large` with `{"error":"directory_too_large"}`. Invalid path
syntax returns `400`; missing, unsupported-object, or confinement-rejected paths return `404`;
denied filesystem access returns `403`; unexpected host
filesystem failures return `500`. Error bodies contain stable identifiers and
do not include native error text or paths.
