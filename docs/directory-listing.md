# Filesystem API

SpareNode exposes each configured filesystem location through read, upload, directory-creation,
and deletion HTTP routes.
For a location configured as `/api/Documents`:

- `GET /api/Documents` lists the location root.
- `GET /api/Documents/*` lists a directory or serves a regular file identified by the
  wildcard suffix.
- `PUT /api/Documents/*` creates a file from the raw request body.
- `POST /api/Documents/*` creates one empty directory at the requested path.
- `DELETE /api/Documents/*` deletes one file, link, or empty directory.

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

## Upload response

An upload uses its wildcard suffix as the URL-encoded relative destination path. The request body
may use `Content-Length` or chunked transfer coding; both forms pass through the same bounded
decoder and temporary-file sink. The route receives only a completed artifact and publishes it
through the confined upload finalizer. Empty request bodies create empty files.

The location must explicitly set `write = true`; otherwise the route returns `403 Forbidden`.
Successful publication returns `201 Created` with `{"status":"created"}`. Existing destinations
are preserved and return `409 Conflict`. Missing parents return `404`, while invalid destination
syntax returns `400`. Uploads never create parent directories implicitly.

Publication revalidates the destination through `SafePath`, holds a confined native parent handle,
copies into a private sibling stage, flushes it, and atomically publishes without replacement.
Incomplete, disconnected, cancelled, expired, or failed request bodies never invoke the route, and
their temporary artifacts remain under automatic cleanup ownership.

## Directory creation response

Directory creation uses `POST` with an empty request body and the wildcard suffix as the
URL-encoded relative destination. The location must explicitly set `write = true`. A successful
request returns `201 Created` with `{"status":"created"}`.

Only the final directory is created. Its parent must already exist inside the configured root;
missing parents return `404`. Existing files, directories, or links are preserved and return
`409 Conflict`. A nonempty request body or invalid destination syntax returns `400`.

The destination passes through `SafePath`, after which SpareNode opens and revalidates the parent
directory against the configured root. Native creation is exclusive and relative to that stable
parent handle or descriptor, preventing a concurrent path replacement from redirecting the new
directory outside the sandbox.

## Deletion response

Deletion uses `DELETE` with an empty request body and requires the location to explicitly set
`delete = true`. This permission is disabled by default and remains independent from `read` and
`write`. A successful request returns `200 OK` with `{"status":"deleted"}`.

File deletion and empty-directory deletion are supported. Recursive deletion is intentionally
unsupported: a non-empty directory is preserved and returns `409 Conflict`. The configured root
cannot be deleted. A final symbolic link or junction is deleted as an entry rather than following
it, but its target must first pass `SafePath` confinement. External, dangling, or unsupported links
are therefore rejected without deleting either the link or its target.

The parent directory is opened and revalidated against the configured root. On Windows, a
read-handle directory oplock blocks relocation of the parent or any ancestor until deletion
finishes. On Linux, deletion runs in an isolated thread whose Landlock policy grants removal only
beneath the retained shared-root descriptor. If the kernel does not provide Landlock, deletion
fails closed. Native paths and system error details are never included in the HTTP response.
