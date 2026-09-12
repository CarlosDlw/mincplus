# Path joining, canonicalization, and file identity. Platform-specific code is
# confined to this leaf (as in support/term), so nothing else has to know how
# identity is decided on a given filesystem.
minc_add_library(minc_fs
  SOURCES
    fs.cc
)
