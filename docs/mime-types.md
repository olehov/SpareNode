# MIME type configuration

SpareNode can load extension-to-media-type mappings from a dedicated file during
startup. Select the file in the `server` block:

```conf
server {
    mime_types_file "mime.types";

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

An absolute path is used directly. A relative path is resolved from the directory
containing the selected `spnode.conf`, independently of the process working
directory. The file is read once during startup and remains immutable while the
server runs. Restart SpareNode after changing it. Omitting `mime_types_file` creates
an empty registry, so every unprotected extension uses the safe
`application/octet-stream` fallback.

## File format

Each nonempty line contains a parameter-free HTTP media type followed by one or
more filename extensions separated by spaces or tabs:

```text
# media-type                 extensions
image/jpeg                   jpeg jpg
text/markdown                md markdown
text/plain                   txt log
```

The leading dot on an extension is optional. Extensions and media types are
matched case-insensitively and normalized to lowercase. `#` starts a comment,
including after a mapping. Semicolons, quoted values, MIME parameters, wildcards,
and compound extensions such as `tar.gz` are not part of this format.

Every extension may appear only once after normalization. Valid extension bytes
are ASCII letters, decimal digits, `+`, `-`, and `_`. A media type must contain
exactly one slash and valid HTTP token bytes on both sides, for example
`application/json`.

## Resource limits

The loader rejects the entire configuration before the server starts when any of
these limits is exceeded:

| Resource | Limit |
| --- | ---: |
| MIME file size | 256 KiB |
| Physical line length | 1024 bytes |
| Expanded extension mappings | 4096 |
| Extension length, excluding an optional dot | 32 bytes |
| Media type length | 127 bytes |

One line with three extensions counts as three mappings. Startup diagnostics name
the MIME file and the failing one-based line without echoing its contents.

## Browser safety policy

File responses continue to use `X-Content-Type-Options: nosniff`. Unknown
extensions use `application/octet-stream`.

Mappings cannot opt browser-executable formats into direct same-origin execution.
Active extensions such as HTML, SVG, JavaScript, CSS, XML, and XSL are served as
`text/plain; charset=utf-8` even if the file assigns another type. Likewise, a
custom extension mapped to an active media type such as `text/html`,
`image/svg+xml`, `application/javascript`, or `application/wasm` is forced to
plain text. This safety rule is part of the server and cannot be disabled through
the MIME file.
