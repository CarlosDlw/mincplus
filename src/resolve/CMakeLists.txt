# Name resolution: scopes, definitions, and one reference per name.
#
# It links `minc_ast` for the lowered tree and `minc_source` only to render a
# location in a dump. It links no diagnostics: errors are values, so a test can
# resolve a unit with no `Session` and no terminal.
minc_add_library(minc_resolve
  SOURCES
    resolve.cc
    lookup.cc
    suggestions.cc
    source_to_def.cc
    store.cc
    dump.cc
    def.cc
    def_index.cc
    predefined.cc
    resolve_error.cc
  DEPS minc_ast minc_builtins minc_intern minc_span minc_source minc_text minc_typenames
)

# Both error tables -> diagnostics. Separate target on purpose: a consumer that
# only wants scopes (the tests, an IDE cache) does not link `DiagBag` at all.
minc_add_library(minc_resolve_report
  SOURCES
    resolve_report.cc
  DEPS minc_resolve minc_diag
)
