# Configuration lexer

`ConfigLexer` is the lexical boundary between UTF-8 `spnode.conf` source and
`ConfigParser`. It recognizes identifiers, strings, integers,
booleans, braces, semicolons, and explicit end-of-input while consuming comments
and whitespace. It does not validate directive names, block structure, duplicate
settings, ports, permissions, or filesystem paths.

## Ownership and lifetime

The lexer borrows a `std::string_view` containing the complete configuration.
Returned token lexemes are views into that same source. The owning string must
therefore remain alive and unmodified until the lexer and all tokens have been
discarded. Decoded string values are owned by their string tokens because escape
processing can produce text that does not exist contiguously in the source.

## Limits and failure behaviour

Input is limited to 1 MiB before tokenization begins. This is an explicit guard
against accidentally processing an unbounded local file; it is not a parser or
filesystem-path limit.

Lexical failures contain a strongly typed category, zero-based byte offset,
one-based line and Unicode scalar column, and bounded diagnostic context. The
first failure becomes terminal, so every later `next()` call returns the same
error rather than advancing through malformed input.

An optional UTF-8 byte-order mark is consumed only at file start. Malformed
UTF-8, embedded nulls, later byte-order marks, unsupported string escapes, raw
line endings in strings, and characters that cannot begin a token are rejected.
LF and CRLF each advance the source by one logical line.

## Token contract

`ConfigToken::lexeme` preserves the exact source bytes. For strings this includes
the surrounding quotes and escape spelling. `ConfigToken::decoded_string` is
populated only for a `string_literal` and contains the once-decoded value.

The lexer classifies lowercase `true` and `false` as `boolean_literal`; all other
valid identifier spellings remain `identifier`, including unknown directive
names. Integer conversion and range checks are deferred to later configuration
stages.
