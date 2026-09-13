# The lowered AST: lowering, structural validation, and the dump.
#
# It reads a green tree and produces values. It links no diagnostics and no
# filesystem: lowering interns names through the session's interner, which is
# passed in, so this module has no state of its own and no platform dependence.
#
# The dump is part of the module rather than of the driver because it is the
# definition of what lowering produced -- tests compare against it, so it cannot
# live behind a target only the driver links.
minc_add_library(minc_ast
  SOURCES
    lower.cc
    validate.cc
    dump.cc
    ast.cc
    ast_error.cc
  DEPS minc_syntax minc_lex minc_intern minc_span
)
