# One-line target definitions. Centralizes include dirs, C++ standard, and
# warning flags so per-module CMakeLists stay tiny and consistent.
#
#   minc_add_library(minc_span SOURCES span_ops.cc DEPS minc_line)
#   minc_add_executable(mincc SOURCES main.cc DEPS minc_driver)
#
# Every target sees ${PROJECT_SOURCE_DIR}/include and the generated include
# dir, so "support/..." and "driver/version.h" resolve everywhere.

# Applies the common include dirs, C++ standard, and warnings to a target.
function(minc_target_configure target visibility)
  target_include_directories(${target} ${visibility}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated
  )
  minc_target_cxx(${target})
  minc_target_warnings(${target})
endfunction()

function(minc_add_library name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
  if(NOT M_SOURCES)
    message(FATAL_ERROR "minc_add_library(${name}): no SOURCES given")
  endif()
  add_library(${name} STATIC ${M_SOURCES})
  minc_target_configure(${name} PUBLIC)
  if(M_DEPS)
    target_link_libraries(${name} PUBLIC ${M_DEPS})
  endif()
  # Namespaced alias: consumers can link minc::minc_span and stay decoupled
  # from the on-disk target name.
  add_library(minc::${name} ALIAS ${name})
endfunction()

function(minc_add_executable name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
  if(NOT M_SOURCES)
    message(FATAL_ERROR "minc_add_executable(${name}): no SOURCES given")
  endif()
  add_executable(${name} ${M_SOURCES})
  minc_target_configure(${name} PRIVATE)
  if(M_DEPS)
    target_link_libraries(${name} PRIVATE ${M_DEPS})
  endif()
endfunction()
