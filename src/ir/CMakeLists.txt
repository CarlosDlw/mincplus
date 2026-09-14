# IR: a checked unit in, an LLVM module out.
#
# This is one of the two targets in the tree that may include `llvm/*` (the other
# is `src/backend`, when it exists), and the isolation is enforced by the build
# graph rather than by a comment: LLVM is attached here with `PRIVATE`, so a
# target that links `minc_ir` inherits no include directory and no definition
# from it. A unit test greps the tree as well, so the rule fails in CI instead of
# in review.
#
# `SYSTEM` on those include directories is not cosmetic: LLVM's headers are
# third-party code and this project builds with `-Wconversion`, `-Wsign-conversion`
# and `-Wundef` as errors in CI, so a warning inside an LLVM header would fail
# this project's build.
#
# The module depends on the artifacts it consumes and on nothing that prints.
# The pattern is the one every stage above follows: a library for the stage and a
# separate one for its diagnostics, so a consumer that only wants a module never
# links `DiagBag`.
minc_add_library(minc_ir
  SOURCES
    lower.cc
    debug.cc
    types.cc
    declarations.cc
    function.cc
    stmt.cc
    expr.cc
    runtime.cc
    invariants.cc
    dump.cc
  DEPS minc_ast minc_resolve minc_sema minc_intern minc_span minc_consteval
)
minc_target_llvm(minc_ir PRIVATE)

# IR refusals -> diagnostics. Separate, for the same reason as every stage above.
minc_add_library(minc_ir_report
  SOURCES
    ir_report.cc
  DEPS minc_ir minc_diag
)
