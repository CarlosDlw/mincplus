# Sanitizer support, off by default.
#
# One switch for the whole build rather than per-target flags: a sanitizer build
# that misses one library silently loses coverage for everything that links it,
# which is exactly the failure mode this is meant to prevent.
#
# Applied at directory scope before any target exists, so every target in src/
# and tests/ is instrumented without each CMakeLists having to remember.

option(MINC_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(minc_enable_sanitizers)
  if(NOT MINC_ENABLE_SANITIZERS)
    return()
  endif()
  if(MSVC)
    # cl.exe can do ASan but not UBSan, and the two cannot be combined there.
    # Better to say so than to half-instrument and imply full coverage.
    message(STATUS "Sanitizers not enabled for MSVC (no UBSan); see docs/roadmap.md")
    return()
  endif()
  # -fno-sanitize-recover: an undefined-behaviour finding aborts the run instead
  # of printing and continuing, so it cannot get lost in a wall of output.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
endfunction()
