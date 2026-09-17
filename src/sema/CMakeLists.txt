# Type checking: a resolved unit in, a typed unit out.
#
# It links `minc_ast` and `minc_resolve` for the artifacts it consumes, and
# `minc_text`/`minc_consteval` for the two things it shares with every other
# stage: the one edit distance and the one constant integer. It links no
# diagnostics and no filesystem -- errors are values -- and it reads the target
# from `sema/target.h`, a table, so there is no platform branch anywhere in this
# module and a cross build types `long` the way the *target* means it.
#
# The dump is part of the module rather than of the driver because it is the
# definition of what checking produced; tests compare against it.
minc_add_library(minc_sema
  SOURCES
    sema.cc
    type.cc
    type_store.cc
    target.cc
    typespec.cc
    convert.cc
    coerce.cc
    access.cc
    check_stmt.cc
    check_expr.cc
    builtins.cc
    check_flow.cc
    global.cc
    alias.cc
    generics.cc
    sema_error.cc
    dump.cc
  DEPS minc_ast minc_builtins minc_resolve minc_intern minc_span minc_text minc_constraint
       minc_consteval
)

# Typing errors -> diagnostics. A separate target for the same reason as every
# stage above: a consumer that only wants a `TypedFile` never links `DiagBag`.
minc_add_library(minc_sema_report
  SOURCES
    sema_report.cc
  DEPS minc_sema minc_diag
)
