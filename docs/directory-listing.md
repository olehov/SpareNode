# Directory listing API

SpareNode exposes the sole configured share through two read-only HTTP routes:

- `GET /api/files` lists the share root.
- `GET /api/files/*` lists the directory identified by the wildcard suffix.

The suffix is an untrusted, URL-encoded UTF-8 relative path. It passes through
`SafePath` before any directory operation. A share with `read = false` returns
`403 Forbidden` without inspecting the directory.

## Response

A successful request returns `application/json; charset=utf-8` and a stable,
name-sorted array:

```json
{
  "share": "Documents",
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
contain basenames and the configured share name; native host paths are never
included.

## Safety and limits

Each child passes through `SafePath` independently. Entries that resolve outside
the configured root, cross an unsupported Windows reparse point, or cannot be
represented as a safe public path are omitted. This keeps one unsafe host entry
from making the rest of a directory unavailable and avoids disclosing its
metadata.

One request inspects at most 512 host entries. A larger directory returns
`413 Content Too Large` with `{"error":"directory_too_large"}`. Invalid path
syntax and non-directory targets return `400`; missing or confinement-rejected
paths return `404`; denied filesystem access returns `403`; unexpected host
filesystem failures return `500`. Error bodies contain stable identifiers and
do not include native error text or paths.
