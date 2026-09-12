add_subdirectory(span)
add_subdirectory(line)
add_subdirectory(source)
add_subdirectory(term)
add_subdirectory(diag)
add_subdirectory(mem)
add_subdirectory(intern)
add_subdirectory(utf8)
add_subdirectory(session)
add_subdirectory(fs)

add_library(minc_support INTERFACE)
target_link_libraries(minc_support INTERFACE
  minc_span
  minc_line
  minc_source
  minc_term
  minc_diag
  minc_mem
  minc_intern
  minc_utf8
  minc_session
  minc_fs
)
target_include_directories(minc_support INTERFACE ${PROJECT_SOURCE_DIR}/include)
