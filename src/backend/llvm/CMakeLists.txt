# Codegen: a module in, an object out, and the two commands that reach a user.
#
# This is the second target in the tree that may include `llvm/*` (the first is
# `minc_ir`), and the isolation is enforced the same way: LLVM is attached with
# `PRIVATE`, so a consumer of `minc_backend` inherits no include directory and no
# definition from it, and a unit test greps the tree. `SYSTEM` on the include
# directories is not cosmetic -- LLVM's headers are third-party code and this
# project builds with `-Wconversion` and `-Wundef` as errors in CI.
#
# The split is the same one every stage above uses: a library for the stage and a
# separate one for its diagnostics, so a consumer that only wants an object never
# links `DiagBag`.
minc_add_library(minc_backend
  SOURCES
    build_info.cc
    diagnostics.cc
    target.cc
    codegen.cc
    link.cc
    run.cc
    files.cc
  DEPS minc_ir minc_sema minc_span minc_fs minc_process
)
minc_target_llvm(minc_backend PRIVATE)

# Backend refusals -> diagnostics.
minc_add_library(minc_backend_report
  SOURCES
    backend_report.cc
  DEPS minc_backend minc_diag
)
