# The builtin table: the identity, the rows and the words.
#
# The one library in this tree with no dependencies -- not even `minc_support`,
# and that is a property of the module rather than an accident. `resolve` binds
# the rows, `sema` types them, `ir` lowers them, the driver renders them and a
# documentation page is generated from them; a module that four stages read may
# not pull any of them in, or the first reader to add a convenient include turns
# the others into its transitive consumers. `include/builtins/builtin.h` says
# the long version.
minc_add_library(minc_builtins
  SOURCES
    table.cc
    names.cc
    dump.cc
)
