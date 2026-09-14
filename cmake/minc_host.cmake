# The host triple, computed once at configure time and baked into a generated
# header (`cmake/host.h.in` -> `<binary-dir>/generated/sema/host.h`).
#
# Why a build-time constant rather than a runtime question: `sema` may not link
# LLVM (only `src/ir` and `src/backend` may), and it may not read the environment
# or a compiler macro, because `--target` has to mean the same thing on every
# machine for a given command line. What *is* host-dependent is which triple the
# compiler was built for, and that is a property of the build, so CMake states it.
#
# The mapping is deliberately a whitelist. `CMAKE_SYSTEM_PROCESSOR` and
# `CMAKE_SYSTEM_NAME` have several spellings per platform, and a table that
# accepted all of them would be guessing about a machine whose ABI `sema` has no
# row for. Anything not below leaves the value empty, prints one warning, and
# makes `--target` mandatory on that build.

set(MINC_HOST_ARCH "")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" MINC_HOST_PROCESSOR)
if(MINC_HOST_PROCESSOR MATCHES "^(x86_64|amd64|x64)$")
  set(MINC_HOST_ARCH "x86_64")
elseif(MINC_HOST_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(MINC_HOST_ARCH "aarch64")
elseif(MINC_HOST_PROCESSOR MATCHES "^riscv64$")
  set(MINC_HOST_ARCH "riscv64")
elseif(MINC_HOST_PROCESSOR MATCHES "^(i386|i486|i586|i686|x86)$")
  set(MINC_HOST_ARCH "i386")
endif()

# The OS component and the environment that goes with it. Windows has no default
# environment -- `msvc` and `gnu` are different ABIs there -- so the compiler
# that is building this one decides: MSVC sets `MSVC`, MinGW does not.
set(MINC_HOST_OS "")
set(MINC_HOST_ENV "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(MINC_HOST_OS "linux")
  set(MINC_HOST_ENV "gnu")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(MINC_HOST_OS "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(MINC_HOST_OS "freebsd")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(MINC_HOST_OS "windows")
  if(MSVC)
    set(MINC_HOST_ENV "msvc")
  else()
    set(MINC_HOST_ENV "gnu")
  endif()
endif()

set(MINC_HOST_TRIPLE "")
if(MINC_HOST_ARCH AND MINC_HOST_OS)
  # `arch-vendor-os[-env]`: the same canonical shape `sema::Triple` prints, so the
  # value is a triple `--target` accepts and not just a string that looks like
  # one. The vendor is `unknown` except where the platform's own toolchain says
  # otherwise, and nothing in the ABI depends on it.
  if(MINC_HOST_OS STREQUAL "windows")
    set(MINC_HOST_VENDOR "pc")
  elseif(MINC_HOST_OS STREQUAL "darwin")
    # `apple` is what the platform's own tools print; `unknown` would be a triple
    # no other tool on that machine agrees with.
    set(MINC_HOST_VENDOR "apple")
  else()
    set(MINC_HOST_VENDOR "unknown")
  endif()
  set(MINC_HOST_TRIPLE "${MINC_HOST_ARCH}-${MINC_HOST_VENDOR}-${MINC_HOST_OS}")
  if(MINC_HOST_ENV)
    set(MINC_HOST_TRIPLE "${MINC_HOST_TRIPLE}-${MINC_HOST_ENV}")
  endif()
else()
  message(WARNING
    "minc+ has no ABI row for this host (processor '${CMAKE_SYSTEM_PROCESSOR}', "
    "system '${CMAKE_SYSTEM_NAME}'). The default target falls back to "
    "x86_64-unknown-linux-gnu, and every build must pass --target.")
endif()

configure_file(
  ${PROJECT_SOURCE_DIR}/cmake/host.h.in
  ${PROJECT_BINARY_DIR}/generated/sema/host.h
  @ONLY
)

message(STATUS "minc+ host triple: ${MINC_HOST_TRIPLE}")
