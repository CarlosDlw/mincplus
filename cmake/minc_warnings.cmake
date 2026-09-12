# Shared warning flags. Include once per target via minc_target_warnings(<tgt>).
# Cross-platform: MSVC (/W4) vs GCC/Clang (-Wall -Wextra ...).
# Warnings are errors only when MINC_WARNINGS_AS_ERRORS=ON (CI).

option(MINC_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

function(minc_target_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(MINC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wcast-qual
      -Wformat=2
      -Wundef
      -Wimplicit-fallthrough
    )
    if(MINC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
