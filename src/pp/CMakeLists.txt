# The preprocessor.
#
# A *client* of the lexer, not a stage before it: a macro body is stored as
# tokens, `##` re-lexes with the same `lexOne`, and there is no second definition
# of "what is an identifier". It therefore links `minc_lex` and not `minc_parse`:
# the preprocessor has no idea a parser exists, which is what keeps it reusable
# by the `pp` command and by a future language server.
#
# Errors are values here, so no diagnostic machinery is linked either.
minc_add_library(minc_pp
  SOURCES
    preprocessor.cc
    directive.cc
    expand.cc
    builtins.cc
    macro.cc
    expansion_table.cc
    conditionals.cc
    const_expr.cc
    stringify.cc
    include_resolver.cc
    pp_error.cc
  DEPS minc_lex minc_fs minc_session minc_consteval
)

# The adapter that lets the parser read the preprocessed stream. Separate target
# on purpose: only the driver wants both halves, and the dependency direction is
# pp -> parser's interface, never the reverse.
minc_add_library(minc_pp_parse
  SOURCES
    pp_token_source.cc
  DEPS minc_pp minc_parse
)

# Errors -> diagnostics. Kept apart so a consumer that only wants tokens (the
# tests, a tool) does not link the diagnostic machinery at all.
minc_add_library(minc_pp_report
  SOURCES
    pp_report.cc
  DEPS minc_pp minc_diag minc_lex_report
)
