# Shared-root configuration

SpareNode v0.1 exposes exactly one host directory. The directory is selected in
a `.env` file in the process working directory:

```dotenv
# Windows
SPARENODE_SHARED_ROOT=D:\Share
```

```dotenv
# Linux
SPARENODE_SHARED_ROOT=/home/user/share
```

Copy `.env.example` to `.env`, then replace the example value. Values containing
spaces may be enclosed in matching single or double quotes. Blank lines,
comments beginning with `#`, and unrelated variables are supported.

`EnvironmentFile` parses and preserves every `KEY=VALUE` assignment without
knowing which settings SpareNode currently uses. It rejects malformed lines and
duplicates of any variable. `ApplicationConfig` then interprets the variables
it understands, beginning with `SPARENODE_SHARED_ROOT`. Adding another setting
therefore does not require extending the environment-file parser.

The supplied path must already exist and identify a directory. SpareNode
resolves it to a canonical absolute path before storing the configuration. A
missing `.env`, malformed entry, duplicate variable, absent
`SPARENODE_SHARED_ROOT`, regular file, or unresolvable path prevents startup and
produces a diagnostic.

The local `.env` is ignored by Git. `.env.example` documents the required key
without committing a machine-specific filesystem path.

## Security boundary

`SharedRoot` is the configuration-level filesystem boundary. Its constructor is
private, so callers cannot create an instance without validation. Later path
handling must resolve untrusted client paths relative to `SharedRoot`; it must
not accept unrestricted host paths as an equivalent substitute.

Canonicalizing the startup path does not by itself protect individual file
operations from traversal, symbolic-link changes, or Windows reparse points.
Those protections belong to the dedicated `SafePath` and filesystem-security
layers.
