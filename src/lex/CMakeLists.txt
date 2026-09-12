# The raw lexer: tokens, the lossless stream, and the dump.
#
# Deliberately free of any dependency on diagnostics. "Lexing never reports an
# error, it flags one" is then enforced by the build graph instead of being a
# claim in a comment.
minc_add_library(minc_lex
  SOURCES
    token_kind.cc
    token.cc
    lexer.cc
    header_name.cc
    literal_scanner.cc
    token_stream.cc
    token_dump.cc
  DEPS minc_span minc_term
)

# Reporting layer: turns token flags and invalid bytes into diagnostics. Kept
# separate so a consumer that only wants tokens (the dump, an editor, a test)
# does not link the diagnostic machinery at all.
minc_add_library(minc_lex_report
  SOURCES lex_report.cc
  DEPS minc_lex minc_diag
)
