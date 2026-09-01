# Shared-root configuration

SpareNode v0.1 exposes exactly one host directory. The directory is selected by
the required `path` directive inside the sole `share` block of `spnode.conf`:

```conf
# Windows
server {
    share "Documents" {
        path "D:\\Share";
    }
}
```

```conf
# Linux
server {
    share "Documents" {
        path "/home/user/share";
    }
}
```

Copy `config/spnode.conf.example` to `config/spnode.conf`, update the share path,
and start SpareNode with `--config config/spnode.conf`. Configuration strings use
double quotes; backslashes must be escaped, while forward slashes are also valid
on Windows. See the [configuration format](configuration-format.md) for the full
grammar and supported directives.

The supplied path must already exist and identify a directory. SpareNode
resolves it to a canonical absolute path before creating runtime settings. A
missing configuration file, absent or duplicate `path` directive, regular file,
or unresolvable path prevents startup and produces a source-located diagnostic.
The local `config/spnode.conf` is ignored by Git so machine-specific paths are
not committed.

## Security boundary

`SharedRoot` is the configuration-level filesystem boundary. Its constructor is
private, so callers cannot create an instance without validation. Later path
handling must resolve untrusted client paths relative to `SharedRoot`; it must
not accept unrestricted host paths as an equivalent substitute.

Canonicalizing the startup path does not by itself protect individual file
operations. The [`SafePath` boundary](safe-path.md) adds lexical confinement for
untrusted relative paths. Symbolic-link changes, Windows reparse points, and
operation-time filesystem races remain the responsibility of the dedicated
filesystem-security layers.
