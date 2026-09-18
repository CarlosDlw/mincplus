# Starting a program and waiting for it. Platform-specific code is confined to
# this leaf (as in support/term and support/fs), so no caller has to know whether
# the platform answers with `posix_spawn` or `CreateProcessW` -- or, more to the
# point, whether either of them reserved two exit statuses for its own use.
minc_add_library(minc_process
  SOURCES
    process.cc
)
