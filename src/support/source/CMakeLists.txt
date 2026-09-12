minc_add_library(minc_source
  SOURCES source_file.cc source_manager.cc file_io.cc
  DEPS minc_line minc_span minc_utf8
)
