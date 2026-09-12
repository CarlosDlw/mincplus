# Centralizes the C++ standard and compiler-level setup for every target.
# One place to change when the project moves to a newer standard.

function(minc_target_cxx target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
  if(MSVC)
    # MSVC assumes the active ANSI code page for narrow sources. Every file
    # here is UTF-8 (comments and string literals), so it must be told.
    # /Zc:__cplusplus makes __cplusplus report the real standard value.
    target_compile_options(${target} PRIVATE /utf-8 /Zc:__cplusplus)
  endif()
endfunction()
