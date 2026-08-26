# Configuration parser

`ConfigParser` consumes the token stream produced by `ConfigLexer` and builds an
owned, typed representation of the version-one `spnode.conf` grammar. It parses
the top-level `server` block, server directives, `share` blocks, share
directives, braces, values, and semicolon terminators.

## Stage boundary

The parser is a syntactic stage. It performs no filesystem or network access and
does not construct runtime server objects. It rejects unknown or misplaced
directive names because the normative grammar does not permit them, but leaves
cardinality and value policy to semantic validation. For example, the parser:

- preserves duplicate directives and multiple `share` blocks in source order;
- accepts a syntactic integer such as port `65536` for later range validation;
- does not check whether a path exists or whether an address can be bound;
- converts integer text to `std::uint64_t` and reports representation overflow.

This separation lets the later validator diagnose all semantic rules against
the original locations without reparsing source text.

## Parsed representation

`ParsedConfiguration` owns its strings and scalar values, so it does not borrow
the lexer's source buffer after parsing completes. A server block contains its
directives and share blocks in separate vectors. Each share retains its decoded
display name and its directives. Directive values use
`directives::ParsedConfigScalar`, a variant of `std::string`, `std::uint64_t`, and `bool`.

The representation stores locations for block keywords, share names, directive
names, scalar values, closing braces, and end-of-input. Closing-brace locations
are especially important for reporting a required directive that is absent.

Server and share directives use separate `ServerDirectiveKind` and
`ShareDirectiveKind` enums, and separate parsed directive structures. This makes
invalid cross-block combinations unrepresentable in the parsed model instead of
placing every directive name into one overly broad enum.

## Failures

`ConfigParserError` distinguishes lexical failures, unexpected tokens, and
integer representation overflow. Grammatical failures carry the expected syntax,
actual token kind, and source location. An unknown identifier is copied into the
error, while string literal contents are not copied into grammatical parser
failures because configuration values may later contain sensitive data. Wrapped
lexer failures retain the lexer's bounded diagnostic context, which can contain
the bytes surrounding malformed string input and must therefore be handled as
potentially sensitive diagnostic data.

The parser is iterative over the grammar's two fixed block levels. Every
successful loop iteration consumes a complete directive or share block; malformed
input either advances to a specific failure or returns immediately. This avoids
unbounded recursion and no-progress loops.
