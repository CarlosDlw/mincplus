minc_add_library(minc_diag
  SOURCES severity.cc diag_bag.cc diag_renderer.cc
  DEPS minc_source minc_span minc_utf8
)
