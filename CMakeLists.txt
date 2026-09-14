cmake_minimum_required(VERSION 3.28)

project(mincplus VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# No translation unit here uses C++20 modules, so there is nothing to scan for.
# Left on, the scan puts a dependency file in every compile command (`@...modmap`
# with GCC as well as Clang), and no tool that reads `compile_commands.json` --
# clang-tidy above all -- can parse those flags. Turning it off is what makes the
# compile database mean what it says. Guarded because the variable is 3.28+.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

# Libraries are static today but position-independent so they can later be
# linked into a shared libminc without a rebuild.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(minc_cxx)
include(minc_warnings)
include(minc_sanitizers)
include(minc_module)
include(GNUInstallDirs)

# Before any target exists, so instrumentation cannot be missed by a module.
minc_enable_sanitizers()

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

# The host triple, in the same generated form and for the same reason: it depends
# on where this compiler is being *built*, and `sema/target.h` may not derive it
# from a preprocessor macro (`--target` would then mean something different on
# each host, and a cross build would be silently aimed at the build machine).
#
# CMake knows the target processor rather than the machine CMake runs on, which
# is what we want: a compiler cross-built for aarch64 is a compiler whose host is
# aarch64.
#
# `CMAKE_SYSTEM_PROCESSOR` is only loosely standardized -- `x86_64` and `AMD64`
# are the same machine, `arm64` and `aarch64` differ only in who is speaking -- so
# the mapping happens here, once, and produces the canonical spelling the table
# states. An architecture or system this compiler has no ABI row for leaves the
# value empty with a warning rather than a guess, and `sema` then falls back to
# its fixed default; `--target` is required on such a host, which is honest.
include(minc_host)


# LLVM is the IR and the backend, and it is isolated on purpose: `src/ir` and
# `src/backend` are the only targets that may include `llvm/*`, and a unit test
# greps the tree so the rule fails in CI instead of in review.
#
# It is REQUIRED rather than optional. An optional dependency that silently turns
# a stage off would make the compiler's behaviour a property of the build
# environment -- the same defect `sema/target.h` refuses when it declines to guess
# a triple. A checkout without LLVM development files cannot build the compiler,
# and saying so at configure time is the honest failure.
find_package(LLVM CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

# LLVM's *build configuration* is part of its ABI, and these are the settings
# that bite across the boundary: RTTI off on one side is an ODR violation rather
# than a link error, and unwinding across a boundary that is not compiled for it
# is undefined. `LLVM_ENABLE_ABI_BREAKING_CHECKS` (implied by
# `LLVM_ENABLE_ASSERTIONS`) changes the *layout* of LLVM's own structs, and the
# macro has to match; this project defines neither, so an LLVM built with them on
# is a configuration this compiler cannot be right about.
if(NOT LLVM_ENABLE_RTTI)
  message(FATAL_ERROR
    "LLVM was built without RTTI and this compiler is built with it. Matching "
    "LLVM's configuration is part of the ABI: a mismatch is an ODR violation "
    "across the LLVM boundary, not a link error.")
endif()
if(LLVM_ENABLE_EH)
  message(FATAL_ERROR
    "LLVM was built with exceptions enabled while this compiler is not; "
    "unwinding across that boundary is undefined behaviour.")
endif()
if(LLVM_ENABLE_ASSERTIONS)
  message(FATAL_ERROR
    "LLVM was built with assertions on, which enables "
    "LLVM_ENABLE_ABI_BREAKING_CHECKS and changes the layout of LLVM's own "
    "types. Build against an LLVM with assertions off, or add the macro to "
    "this project's definitions.")
endif()

# One function so every target that talks to LLVM gets the same include dirs,
# the same definitions and the same library. `SYSTEM` is not cosmetic: LLVM's
# headers are third-party code, and this project compiles with -Wconversion,
# -Wsign-conversion and -Wundef as errors in CI. Marking the directory SYSTEM is
# what keeps a warning inside an LLVM header from failing this project's build.
function(minc_target_llvm target visibility)
  # `LLVM_DEFINITIONS` is one space-separated string, not a CMake list, and
  # `target_compile_definitions` takes a list. Handing the string straight over
  # makes CMake treat it as a single value and quote it whole, which is wrong in
  # two ways that do not look wrong in a build log: `-D_GLIBCXX_USE_CXX11_ABI=1`
  # swallowed the three `__STDC_*` defines into its replacement list, so they
  # were never defined, and `_GLIBCXX_USE_CXX11_ABI` itself got a garbage value --
  # an ABI-affecting macro to get wrong. Splitting it restores one define per
  # word; `-D` is stripped by `target_compile_definitions` itself.
  separate_arguments(llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_include_directories(${target} SYSTEM ${visibility} ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(${target} ${visibility} ${llvm_definitions})
  # The umbrella library. A distribution LLVM is one shared object regardless of
  # how the components are named, and linking the umbrella is what makes a
  # build against it a single link rather than a component list to keep in step.
  target_link_libraries(${target} ${visibility} LLVM)
endfunction()

add_subdirectory(src)

if(MINC_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
endif()

install(TARGETS mincc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
