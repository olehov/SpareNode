# Owner credential provisioning

SpareNode v0.1 loads exactly one owner credential from a separate bounded file. The server never
accepts a plaintext password in `spnode.conf` or the credential file, does not persist plaintext
credentials, and does not include login values or password verifiers in startup diagnostics.

This stage establishes the credential-provider and password-verification boundary. Login routes,
sessions, and protection of filesystem routes are implemented by SN-042 and SN-043. Creating the
file below does not by itself make the current filesystem API authenticated.

## Selecting the credential file

Set the optional server directive in `spnode.conf`:

```text
server {
    credentials_file "credentials.conf";
    # Other server directives and locations...
}
```

An absolute path is used directly. A relative path is resolved from the directory containing
`spnode.conf`. When the directive is omitted, SpareNode uses `credentials.conf` in that directory.
The selected path must identify a regular file directly; symbolic links, directories, devices,
and other special files are rejected. The file must also remain outside every filesystem
`location`, even when that location is read-only, so the HTTP API cannot expose its verifier.
All checks complete before the server opens its listener.

The repository includes `config/credentials.conf.example` to document the generated format. The
real `credentials.conf` is ignored by Git. Keep deployment credential files outside source control
and restrict read and write access to the operating-system account that starts SpareNode. The
first-party provisioning tool described below creates owner-only mode `0600` on POSIX. On Windows,
it creates a protected ACL granting full access only to the file owner, SYSTEM, and administrators.

## Credential file format

The file contains exactly one `owner` block with one login and one encoded verifier:

```text
owner {
    login "admin";
    password_verifier "$argon2id$v=19$m=65536,t=2,p=1$<salt>$<hash>";
}
```

Directive order is not significant. Duplicate and unknown directives, trailing content, invalid
UTF-8, malformed strings, and files larger than 4096 bytes are rejected. The login is
case-sensitive, must contain 1 through 128 bytes, and cannot contain ASCII control bytes.

The verifier must use Argon2id v1.3 with libsodium's interactive profile: 64 MiB of memory, two
operations, and one lane. Its encoded form contains the algorithm, parameters, random salt, and
hash. It is not a plaintext password, but it permits offline password guessing if disclosed, so
treat the complete credential file as a secret.

## Creating the credential file

Run the installed `sparenode-credentials` tool, or its build-tree executable, on the machine that
will store the credential file:

```powershell
.\build\msvc\bin\Debug\sparenode-credentials.exe --output config\credentials.conf
```

The tool asks for the login, password, and password confirmation. Password input is hidden. It
creates the salt and Argon2id verifier, writes the complete owner block, and retains no plaintext
password in the resulting file. The output defaults to `credentials.conf` when `--output` is
omitted. The tool refuses non-interactive password input and refuses to replace an existing file;
move the old file aside deliberately before rotating credentials.

Each run uses a fresh cryptographically secure random salt, so files generated from the same
password are expected to contain different verifiers. Do not pass a plaintext password as a
command-line argument. Command arguments may be retained in shell history or exposed through
process-inspection tools.

## Starting SpareNode

After creating `spnode.conf` and its selected credential file, start the server normally:

```powershell
.\build\msvc\bin\Debug\sparenode.exe --config config\spnode.conf
```

Startup fails before listener creation when the file is missing, unreadable, oversized, malformed,
not a regular non-symlink file, located below a configured filesystem location, is missing either
required directive, or contains an unsupported login or verifier. Diagnostics name only the
failure category and never include credential values.

The concrete file provider implements the generic `CredentialProvider` interface.
`AuthenticationService` receives only that interface and a `PasswordVerifier`, so a future SQLite
or operating-system secret-store provider can reuse the same verification path.

Unknown logins still execute one Argon2id verification against a process-local dummy verifier.
They return the same `invalid_credentials` domain result as an incorrect password. Provider and
verifier failures return only `unavailable` and fail closed.

Temporary submitted passwords are represented by `SensitivePassword`, which owns heap-backed
bytes and erases them with `sodium_memzero` on every destruction or replacement path. Future login
parsing should transfer its bounded password buffer directly into this owner to avoid additional
plaintext copies.

## Cryptographic dependency

SpareNode pins the CMake wrapper and underlying libsodium source revision used by the build. It
uses `crypto_pwhash_str_alg` with explicit Argon2id and interactive parameters,
`crypto_pwhash_str_verify` for verification, and `crypto_pwhash_str_needs_rehash` to reject
malformed or incompatible provisioning material. SpareNode does not implement password hashing,
salt generation, or constant-time hash comparison itself.

The packaged application includes the libsodium license under the installation license directory.

## References

- [libsodium password hashing](https://doc.libsodium.org/password_hashing/default_phf)
