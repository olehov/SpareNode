# Safe-path boundary

`sparenode::filesystem::SafePath` is the first type accepted by filesystem
operations for paths originating outside the process. Callers resolve an
untrusted UTF-8 path relative to a validated `SharedRoot`; they cannot directly
construct a `SafePath` from an unrestricted host path.

## Resolution contract

`SafePath::resolve()` performs these steps:

1. Reject requests larger than 4096 bytes before parsing or conversion.
2. Decode URL percent escapes exactly once and reject malformed or ambiguously
   nested encoding.
3. Normalize slash and backslash to one portable separator representation.
4. Validate the decoded path as UTF-8 and reject decoded or literal null bytes.
5. Reject slash-rooted, UNC-style, and Windows drive-qualified input on every
   platform before converting it to `std::filesystem::path`.
6. Convert the decoded request to the platform-native path form.
7. On Windows, reject alternate data stream separators, reserved punctuation,
   control characters, non-special components ending in an ASCII space or
   period, and device names such as `NUL`, `CON`, `COM1`, and their
   extension-bearing aliases before Win32 can reinterpret them.
8. Join the relative request to the canonical shared root.
9. Normalize `.` and `..` components lexically without requiring the target to
   exist.
10. Compare complete path components to ensure the result is the shared root or
   one of its descendants.
11. Recheck the configured root, then inspect each existing request prefix with
    `symlink_status` and canonicalize it. Reject resolved prefixes outside the
    root, dangling links, cycles, and filesystem errors. Append a genuinely
    missing suffix only after its existing parent has passed these checks.
12. On Windows, open each existing component with
    `FILE_FLAG_OPEN_REPARSE_POINT`, read its reparse tag, and allow only symbolic
    links and mount points (the tag used by directory junctions). Read each
    supported target with `FSCTL_GET_REPARSE_POINT`, restart component inspection
    at that target, and reject a chain above Windows' 63-hop limit. This ensures
    that a supported junction cannot conceal a disallowed intermediate or final
    reparse tag.
13. After every redirection has been inspected, obtain the normalized handle
    name with `GetFinalPathNameByHandleW` and retain its `\\?\` prefix. Reject
    handle-derived components ending in a period or space before legacy Win32
    APIs can reinterpret them as a different object. The resulting exact path
    must remain under the handle-derived shared root.

URL path decoding is case-insensitive for hexadecimal digits and does not treat
`+` as a space. A decoded result containing another valid percent escape is
rejected instead of decoded repeatedly. This prevents validation from approving
a path that a later accidental decoding pass could reinterpret as traversal.

Both `/` and `\` are network path separators regardless of the host operating
system. Consequently, mixed-separator traversal has the same result on Windows
and Linux, and Windows drive or UNC syntax cannot become an ordinary Linux file
name at this boundary.

The 4096-byte limit is a SpareNode input policy that bounds work performed on
untrusted data. It is not a statement of the host filesystem's maximum path
length, and successful resolution does not guarantee that every later native
filesystem operation will accept the resulting path.

Containment does not use a string-prefix check. For example, a sibling named
`shared-private` is not considered a child of a root named `shared`.

An empty request resolves to the shared root. A missing final path component is
also allowed when it remains inside the root, enabling later upload and create
operations. The operation consuming `SafePath` remains responsible for its own
requirements, such as whether the target must already exist or be a regular
file.

Internal file and directory symbolic links, including chains, resolve to their
canonical target. An external directory link is rejected even if the requested
child does not exist. Broken links are errors, not prospective upload targets.
Request normalization precedes filesystem lookup: `link/../file.txt` denotes
`file.txt` in the root; callers must use the returned path rather than reopen
the original request. Native canonicalization resolves dots in link targets.

## Errors

Resolution returns `SafePathError` rather than throwing for expected invalid
input. The error distinguishes an oversized request, invalid or nested percent
encoding, invalid UTF-8, embedded null bytes, rooted input, platform-invalid
components, escape from the shared root, and failure to resolve an existing
prefix or link target (`resolution_failed`). On Windows,
`unsupported_reparse_point` distinguishes a reparse tag whose behavior is not
part of SpareNode's supported symlink and junction policy. Oversized input is
not copied into the error object; its `requested_path` field is empty.

## Remaining security work

`SafePath` checks symbolic-link confinement at resolution time. It does not hold
an open file or directory handle, so replacing a directory or link after the
check can invalidate that result. File operations must prevent these races at
open/create time; a `SafePath` alone is not an authorization to follow a mutable
pathname without further protection. Hard links are not redirects and remain
inside this path-based policy. Volume mount points are handled as Windows
reparse points and must resolve inside the shared root.

## Symbolic-link tests

The `[symlink]` tests use real file and directory links on Linux and Windows.
Windows requires Developer Mode or the symbolic-link creation privilege.
Local tests skip only a missing Windows privilege; other setup errors fail.
The Windows CI job configures `-DSPARENODE_REQUIRE_SYMLINK_TESTS=ON` so a missing
privilege fails the suite instead of silently leaving this protection untested.

Windows junction tests do not require the symbolic-link privilege. They create
real `IO_REPARSE_TAG_MOUNT_POINT` objects through `FSCTL_SET_REPARSE_POINT` and
cover internal targets, external targets, nested redirection, missing suffixes,
ordinary directories, replacement of the configured root, a forbidden
third-party target behind an allowed junction, and trailing-period aliases.
