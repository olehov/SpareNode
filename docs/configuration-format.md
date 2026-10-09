# SpareNode configuration format

This document defines version 1 of the persistent `spnode.conf` format. The
repository keeps its distributable example at `config/spnode.conf.example` and
reserves `config/spnode.conf` for a local development configuration. Start the
executable with `sparenode --config /path/to/spnode.conf`. This explicit file is
the sole startup configuration source.

The language is deliberately small. It is not nginx-compatible and does not
support includes, substitutions, expressions, or executable statements.

## Complete example

```conf
# SpareNode accepts direct HTTP only from this host on port 8080.
server {
    bind "127.0.0.1";
    port 8080;
    transport_mode "loopback_http";
    multithreading true;
    worker_threads 4;
    log_level "info";
    mime_types_file "mime.types";
    header_timeout_ms 10000;
    body_timeout_ms 10000;
    request_timeout_ms 30000;

    location "/api/Documents" {
        path "/home/user/Documents";
        read true;
        write false;
        delete false;
    }
}
```

On Windows, the location can instead use an escaped native path:

```conf
server {
    location "/api/Documents" {
        path "D:\\Shared\\Documents";
        read true;
        write false;
        delete false;
    }
}
```

Forward slashes are also valid in Windows path strings, for example
`"D:/Shared/Documents"`. Path existence, canonicalization, platform-specific
validity, and filesystem security are semantic concerns handled after parsing.

## Source encoding and whitespace

- A configuration file is UTF-8. An optional UTF-8 byte-order mark is accepted
  only at the beginning of the file.
- Invalid UTF-8, embedded null bytes, and a byte-order mark anywhere else are
  errors.
- Space, horizontal tab, carriage return, and line feed are whitespace outside
  strings. Both LF and CRLF line endings are accepted.
- Identifiers, keywords, and boolean values are ASCII and case-sensitive.
- Non-ASCII text is permitted inside quoted strings.
- The entire file must conform to the grammar; partial configuration is never
  applied after an error.

## Lexical elements

### Identifiers and keywords

An identifier begins with an ASCII letter or underscore and continues with
ASCII letters, decimal digits, underscores, or hyphens:

```text
identifier = ( ALPHA | "_" ), { ALPHA | DIGIT | "_" | "-" } ;
```

Version 1 reserves `server`, `location`, `bind`, `port`, `multithreading`,
`worker_threads`, `log_level`, `header_timeout_ms`, `body_timeout_ms`,
`request_timeout_ms`, `mime_types_file`, `transport_mode`, `trusted_proxy`, `path`,
`read`, `write`, `delete`, `true`, and `false` according to their grammatical
positions. Keywords must be written in lowercase.

### Strings

Strings use double quotes. The following escape sequences are supported:

| Escape | Decoded value |
|---|---|
| `\"` | double quote |
| `\\` | backslash |
| `\n` | line feed |
| `\r` | carriage return |
| `\t` | horizontal tab |

Every other escape, including `\x` and `\u`, is invalid in version 1. A raw
line ending or null byte may not appear inside a string. An unterminated string
is an error. Escape decoding happens exactly once.

### Integers and booleans

An integer is one or more ASCII decimal digits. A sign, separator, decimal
point, hexadecimal prefix, or suffix is invalid. Leading zeroes are accepted
and do not change the decimal interpretation.

The only boolean literals are the lowercase keywords `true` and `false`.

### Comments

`#` begins a comment outside a string. The comment continues to the next line
ending or end-of-input. A `#` inside a quoted string is ordinary string data.

```conf
# Full-line comment
bind "127.0.0.1"; # End-of-line comment
```

### Punctuation

`{` and `}` delimit blocks. Every directive ends with `;`. Punctuation has no
special meaning inside a quoted string.

## Grammar

The following EBNF is normative. `identifier`, `string`, and `integer` are the
lexical elements defined above. Whitespace and comments may occur between any
two tokens.

```text
configuration            = server-block, end-of-input ;

server-block             = "server", "{", { server-item }, "}" ;
server-item              = bind-directive
                         | port-directive
                         | threading-directive
                         | worker-threads-directive
                         | log-level-directive
                         | mime-types-file-directive
                         | header-timeout-directive
                         | body-timeout-directive
                         | request-timeout-directive
                         | transport-mode-directive
                         | trusted-proxy-directive
                         | location-block ;

bind-directive           = "bind", string, ";" ;
port-directive           = "port", integer, ";" ;
threading-directive      = "multithreading", boolean, ";" ;
worker-threads-directive = "worker_threads", integer, ";" ;
log-level-directive      = "log_level", string, ";" ;
mime-types-file-directive = "mime_types_file", string, ";" ;
header-timeout-directive = "header_timeout_ms", integer, ";" ;
body-timeout-directive   = "body_timeout_ms", integer, ";" ;
request-timeout-directive = "request_timeout_ms", integer, ";" ;
transport-mode-directive = "transport_mode", string, ";" ;
trusted-proxy-directive  = "trusted_proxy", string, ";" ;

location-block              = "location", string, "{", { location-directive }, "}" ;
location-directive          = path-directive
                         | read-directive
                         | write-directive
                         | delete-directive ;

path-directive           = "path", string, ";" ;
read-directive           = "read", boolean, ";" ;
write-directive          = "write", boolean, ";" ;
delete-directive         = "delete", boolean, ";" ;

boolean                  = "true" | "false" ;
```

Directive order within a block is not significant.

## Version 1 semantics

Version 1 requires exactly one top-level `server` block and at least one filesystem
`location` block within it. The quoted location argument is the public HTTP API path
prefix for that filesystem root.

### Server directives

| Directive | Cardinality | Default | Semantic requirement |
|---|---:|---|---|
| `bind` | zero or one | `"127.0.0.1"` | Numeric IPv4 or IPv6 address permitted by the transport mode |
| `port` | zero or one | `8080` | Decimal value from 1 through 65535 |
| `transport_mode` | zero or one | `"loopback_http"` | One of `"loopback_http"`, `"trusted_lan_http"`, or `"trusted_proxy_https"` |
| `trusted_proxy` | conditional | none | One numeric IPv4 or IPv6 address, required only in `trusted_proxy_https` mode |
| `multithreading` | zero or one | `false` | Enables the configured worker pool when `true` |
| `worker_threads` | conditional | none | Required with `multithreading true`; integer from 2 through 64 |
| `log_level` | zero or one | `"info"` | One of `"debug"`, `"info"`, `"warning"`, or `"error"` |
| `mime_types_file` | zero or one | none | Non-empty path to a bounded `mime.types` file |
| `header_timeout_ms` | zero or one | `10000` | Header receive inactivity, 1 through 86400000 milliseconds |
| `body_timeout_ms` | zero or one | `10000` | Body receive inactivity, 1 through 86400000 milliseconds |
| `request_timeout_ms` | zero or one | `30000` | Total request receive time, 1 through 86400000 milliseconds |
| `location` | one or more | none | Defines an HTTP prefix and its filesystem root |

Hostnames are not accepted by `bind` in version 1. IPv6 addresses remain quoted
strings, for example `bind "::";` or `bind "::1";`.

The default `loopback_http` mode accepts only loopback bind and peer addresses.
`trusted_lan_http` explicitly permits plaintext HTTP on a network-facing bind and ignores
forwarding headers. `trusted_proxy_https` accepts requests only from the exact configured
`trusted_proxy`, requires validated HTTPS forwarding metadata, and rejects wildcard bind
addresses. See [Authentication transport and network exposure](authentication-transport.md)
for the threat assumptions, proxy contract, and deployment examples.

An absolute `mime_types_file` path is used directly. A relative path is resolved
from the directory containing `spnode.conf`. The file is loaded once before
server resources start; unreadable or invalid content is fatal. When the directive
is omitted, the runtime registry is empty and file responses use the safe unknown
type fallback. See [MIME type configuration](mime-types.md) for syntax, bounds,
diagnostics, and the browser safety policy.

Receive timeouts use unsigned integer milliseconds without a unit suffix. Zero,
negative values, quoted numbers, and values above 24 hours are rejected before
startup. Omitting these directives preserves the defaults. An inactivity budget
may exceed the total budget; the earlier deadline always wins.

The header budget starts when a dispatcher worker begins handling the connection.
Each nonempty receive renews inactivity. When the header terminator arrives, the
body budget applies from that receive timestamp. The total budget is never renewed,
so a client cannot keep a worker forever by sending occasional bytes. These limits
cover receiving the request, not queue residence, route execution, or response
transmission. See [HTTP connection deadlines](http-connection-handler.md#cancellation-and-deadlines).

When `multithreading` is omitted or `false`, `worker_threads` must be omitted and
the effective worker count is exactly one. When `multithreading` is `true`,
`worker_threads` is required and selects the exact size of the fixed dispatcher
pool. Values outside `2..64` are rejected before any worker starts. The explicit
upper bound prevents an accidental configuration value from attempting to
create an unbounded number of operating-system threads; a later resource-profile
issue may revise this policy deliberately.

### Location directives

| Directive | Cardinality | Default | Semantic requirement |
|---|---:|---|---|
| `path` | exactly one | none | Non-empty host path accepted by `SharedRoot` |
| `read` | zero or one | `true` | Allows file and directory reads |
| `write` | zero or one | `false` | Allows creation and upload |
| `delete` | zero or one | `false` | Allows file and directory deletion |

Permissions are independent. In particular, `delete true` does not implicitly
enable `write`, and `write true` does not implicitly enable `delete`. Filesystem
operations must still pass through the `SharedRoot` and `SafePath` security
boundaries; configuration permissions cannot bypass them.

Location API paths must be absolute origin paths such as `/api/Documents`. A path
cannot contain a query, fragment, backslash, control character, wildcard, empty
segment, `.` segment, or `..` segment, and non-root paths cannot end in `/`.
Duplicate paths and segment-nested paths are rejected because their exact and
descendant routes would overlap. Segment neighbors such as `/api/docs` and
`/api/docs-old` remain distinct.

### Relationship to HTTP requests

The exact configured path addresses the filesystem root. Descendant requests beneath
that prefix are passed to the same location as relative paths and resolved through
`SafePath`. Before an operation starts, its handler must also enforce the corresponding
`read`, `write`, or `delete` permission. The argument is not a display name; UI-facing
metadata may be added later when its requirements are known.

## Duplicate and unknown names

- Repeating a singleton directive in the same block is an error, even when both
  values are identical.
- A second `server` block is an error. Multiple non-conflicting locations are allowed.
- Duplicate or segment-nested location API paths are errors.
- An unknown directive or block is an error. It is not ignored and does not
  produce a warning-only fallback.
- A recognized directive in the wrong block is an error.
- Directive and block names are case-sensitive; `Port` is unknown.

Rejecting unknown and duplicate names prevents misspellings from silently
changing the server's security or network behaviour.

## Invalid examples

Each of the following configurations must fail as a whole.

Missing the required location:

```conf
server {
    port 8080;
}
```

Duplicate directive:

```conf
server {
    port 8080;
    port 9090;

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

Unknown directive:

```conf
server {
    listen_port 8080;

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

Missing statement terminator:

```conf
server {
    port 8080

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

Invalid string escape in a Windows path:

```conf
server {
    location "/api/Documents" {
        path "D:\Shared";
    }
}
```

The last example contains unsupported `\S`; it must use
`path "D:\\Shared";` or `path "D:/Shared";`.

Semantic errors are also fatal, including port `0`, an unsupported log level,
an invalid location API path, an empty filesystem path, or a path that does not identify an
acceptable directory on the host platform.

Unsafe transport combinations are also fatal. For example, the default loopback mode cannot
be combined with `bind "0.0.0.0";`, and proxy mode cannot omit `trusted_proxy`.

A threading configuration is invalid when the switch and count disagree:

```conf
server {
    multithreading false;
    worker_threads 4;

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

Likewise, `multithreading true` without `worker_threads` is invalid rather than
silently selecting a platform-dependent count.

## Diagnostics and processing stages

Configuration processing is divided into explicit stages:

```text
UTF-8 spnode.conf
        |
        v
ConfigLexer         tokens or lexical error
        |
        v
ConfigParser        syntax tree or grammatical error
        |
        v
semantic validation runtime configuration or semantic error
```

Every structured lexer, parser, and semantic error must carry a one-based line
and column pointing at the offending token or character. The presentation layer
associates that location with the source file to produce diagnostics such as
`spnode.conf:7:14: error: unterminated string literal`. Diagnostics should name
the error category and relevant directive without echoing secrets. Later stages
must not reinterpret or decode string escapes a second time.

When a required directive is absent, its semantic error points at the closing
`}` of the block that should contain it. This applies to a missing `location` and to
`worker_threads` when `multithreading true` is present. If the relevant closing
delimiter is unavailable, including when the entire `server` block is absent,
the error points at end-of-input. End-of-input is the position immediately after
the final decoded character: an empty file is `1:1`, and a file ending in a line
break points at column 1 of the following line.

The lexer does not validate directive placement, duplicates, addresses, ports,
permissions, or filesystem paths. The parser does not open filesystem paths.
`SharedRoot` remains responsible for validating and canonicalizing the configured
directory before the server starts.

## Future compatibility

Version 1 intentionally excludes include files, environment-variable expansion,
hot reload, serialization, authentication values, expressions, and UI display
metadata for locations. Adding any of these requires a dedicated issue and an explicit format
revision. A future settings UI must read and write the same configuration model;
it must not introduce an independent source of truth.
