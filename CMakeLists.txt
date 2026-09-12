cmake_minimum_required(VERSION 3.28)

project(mincplus VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Libraries are static today but position-independent so they can later be
# linked into a shared libminc without a rebuild.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(minc_cxx)
include(minc_warnings)
include(minc_module)
include(GNUInstallDirs)

# Fast builds: use ccache when available.
find_program(MINC_CCACHE ccache)
if(MINC_CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER ${MINC_CCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${MINC_CCACHE})
endif()

option(MINC_ENABLE_TESTS "Build unit tests" ON)
option(MINC_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MINC_FETCH_GTEST "Download googletest when it is not installed" ON)

# Single source of truth for the driver version string. Generated into the
# binary dir; the source tree must not carry a second copy.
configure_file(
  ${PROJECT_SOURCE_DIR}/cmake/version.h.in
  ${PROJECT_BINARY_DIR}/generated/driver/version.h
  @ONLY
)

# LLVM is the planned backend but stays isolated in src/backend/llvm.
# The scaffold only detects it; nothing outside the backend may include llvm/*.
find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
else()
  message(STATUS "LLVM not found (backend/llvm will require it later)")
endif()

add_subdirectory(src)

if(MINC_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
endif()

install(TARGETS mincc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
