# The syntax tree: the green tree, the cursor, the typed AST view, and the dump.
minc_add_library(minc_syntax
  SOURCES
    green.cc
    node.cc
    tree.cc
    builder.cc
    ast.cc
    store.cc
    dump.cc
  DEPS minc_parse minc_lex minc_span minc_mem
)
