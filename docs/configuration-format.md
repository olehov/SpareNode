# SpareNode configuration format

This document defines version 1 of the persistent `spnode.conf` format. The
repository keeps its distributable example at `config/spnode.conf.example` and
reserves `config/spnode.conf` for a local development configuration. This is the
lexical and grammatical contract for the configuration lexer and parser; the
current executable continues to read `.env` until those later components are
integrated.

The language is deliberately small. It is not nginx-compatible and does not
support includes, substitutions, expressions, or executable statements.

## Complete example

```conf
# SpareNode listens on every IPv4 interface on port 8080.
server {
    bind "0.0.0.0";
    port 8080;
    multithreading true;
    worker_threads 4;
    log_level "info";

    share "Documents" {
        path "/home/user/Documents";
        read true;
        write false;
        delete false;
    }
}
```

On Windows, the share can instead use an escaped native path:

```conf
server {
    share "Documents" {
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

Version 1 reserves `server`, `share`, `bind`, `port`, `multithreading`,
`worker_threads`, `log_level`, `path`, `read`, `write`, `delete`, `true`, and
`false` according to their grammatical positions. Keywords must be written in
lowercase.

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
                         | share-block ;

bind-directive           = "bind", string, ";" ;
port-directive           = "port", integer, ";" ;
threading-directive      = "multithreading", boolean, ";" ;
worker-threads-directive = "worker_threads", integer, ";" ;
log-level-directive      = "log_level", string, ";" ;

share-block              = "share", string, "{", { share-directive }, "}" ;
share-directive          = path-directive
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

Version 1 requires exactly one top-level `server` block and exactly one `share`
block within it. The share name must decode to a non-empty string. The name is a
user-facing label and is not a filesystem path.

### Server directives

| Directive | Cardinality | Default | Semantic requirement |
|---|---:|---|---|
| `bind` | zero or one | `"0.0.0.0"` | Numeric IPv4 or IPv6 address |
| `port` | zero or one | `8080` | Decimal value from 1 through 65535 |
| `multithreading` | zero or one | `false` | Enables the configured worker pool when `true` |
| `worker_threads` | conditional | none | Required with `multithreading true`; integer from 2 through 64 |
| `log_level` | zero or one | `"info"` | One of `"debug"`, `"info"`, `"warning"`, or `"error"` |
| `share` | exactly one | none | Defines the sole MVP shared directory |

Hostnames are not accepted by `bind` in version 1. IPv6 addresses remain quoted
strings, for example `bind "::";` or `bind "::1";`.

When `multithreading` is omitted or `false`, `worker_threads` must be omitted and
the effective worker count is exactly one. When `multithreading` is `true`,
`worker_threads` is required and selects the exact size of the fixed dispatcher
pool. Values outside `2..64` are rejected before any worker starts. The explicit
upper bound prevents an accidental configuration value from attempting to
create an unbounded number of operating-system threads; a later resource-profile
issue may revise this policy deliberately.

### Share directives

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

The single-share restriction belongs to the version 1 semantic model rather
than the lexical grammar. A future version may permit multiple uniquely named
`share` blocks without changing how a block is tokenized.

### Relationship to HTTP requests

The quoted share name is a display label, not an HTTP route identifier. Because
version 1 has exactly one share, file endpoints select that share from the
validated runtime configuration and pass only the request's relative file path
through `SafePath`. Before an operation starts, its handler must also enforce the
corresponding `read`, `write`, or `delete` permission.

HTTP routing and handlers remain outside this format specification. A future
multi-share configuration should introduce a separate stable share identifier
and routes such as `/api/shares/{share_id}/files/...`. Using the display label as
that identifier would make URLs unstable under renaming and introduce avoidable
Unicode and percent-encoding ambiguity.

## Duplicate and unknown names

- Repeating a singleton directive in the same block is an error, even when both
  values are identical.
- A second `server` block or a second `share` block is an error in version 1.
- An unknown directive or block is an error. It is not ignored and does not
  produce a warning-only fallback.
- A recognized directive in the wrong block is an error.
- Directive and block names are case-sensitive; `Port` is unknown.

Rejecting unknown and duplicate names prevents misspellings from silently
changing the server's security or network behaviour.

## Invalid examples

Each of the following configurations must fail as a whole.

Missing the required share:

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

    share "Documents" {
        path "/srv/documents";
    }
}
```

Unknown directive:

```conf
server {
    listen_port 8080;

    share "Documents" {
        path "/srv/documents";
    }
}
```

Missing statement terminator:

```conf
server {
    port 8080

    share "Documents" {
        path "/srv/documents";
    }
}
```

Invalid string escape in a Windows path:

```conf
server {
    share "Documents" {
        path "D:\Shared";
    }
}
```

The last example contains unsupported `\S`; it must use
`path "D:\\Shared";` or `path "D:/Shared";`.

Semantic errors are also fatal, including port `0`, an unsupported log level,
an empty share name, an empty path, or a path that does not identify an
acceptable directory on the host platform.

A threading configuration is invalid when the switch and count disagree:

```conf
server {
    multithreading false;
    worker_threads 4;

    share "Documents" {
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
`}` of the block that should contain it. This applies to a missing `share` and to
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
hot reload, serialization, authentication values, expressions, and multiple
shares. Adding any of these requires a dedicated issue and an explicit format
revision. A future settings UI must read and write the same configuration model;
it must not introduce an independent source of truth.
