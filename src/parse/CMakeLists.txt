# The parser: a token source in, events and errors out.
#
# Deliberately free of any dependency on diagnostics or on the tree. "The parser
# does not report and does not build a node" is then enforced by the build graph
# instead of being a claim in a comment, and the grammar can be tested with no
# `Session` in sight.
minc_add_library(minc_parse
  SOURCES
    syntax_kind.cc
    parse_error.cc
    token_spelling.cc
    token_source.cc
    parser.cc
    declaration.cc
    statement.cc
    expression.cc
  DEPS minc_lex minc_span
)

# Reporting layer: turns syntax errors into diagnostics. Kept separate so a
# consumer that only wants the grammar (a test, an editor) does not link the
# diagnostic machinery at all.
minc_add_library(minc_parse_report
  SOURCES parse_report.cc
  DEPS minc_parse minc_diag
)
