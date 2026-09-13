# Unit tests. GTest discovery is layered so the suite builds on every
# platform: CMake package config first, then pkg-config, then a pinned
# FetchContent download (opt out with -DMINC_FETCH_GTEST=OFF).

find_package(GTest CONFIG QUIET)

if(NOT TARGET GTest::gtest_main)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GTEST_PC IMPORTED_TARGET gtest)
    pkg_check_modules(GTEST_MAIN_PC IMPORTED_TARGET gtest_main)
    if(TARGET PkgConfig::GTEST_PC)
      add_library(GTest::gtest INTERFACE IMPORTED)
      target_link_libraries(GTest::gtest INTERFACE PkgConfig::GTEST_PC)
      add_library(GTest::gtest_main INTERFACE IMPORTED)
      target_link_libraries(GTest::gtest_main INTERFACE GTest::gtest)
      if(TARGET PkgConfig::GTEST_MAIN_PC)
        target_link_libraries(GTest::gtest_main INTERFACE PkgConfig::GTEST_MAIN_PC)
      endif()
    endif()
  endif()
endif()

if(NOT TARGET GTest::gtest_main AND MINC_FETCH_GTEST)
  message(STATUS "GTest not found locally; fetching googletest v1.17.0")
  include(FetchContent)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # single CRT on MSVC
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(NOT TARGET GTest::gtest_main)
  message(FATAL_ERROR
    "GTest was not found. Install it or configure with -DMINC_FETCH_GTEST=ON.")
endif()

# The example regression test needs the source tree's `examples/` directory.
# Generated rather than passed as a -D so a path with spaces or backslashes
# survives every generator and toolchain.
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/examples" MINC_EXAMPLES_DIR)
configure_file(
  ${PROJECT_SOURCE_DIR}/cmake/examples_dir.h.in
  ${PROJECT_BINARY_DIR}/generated/tests/examples_dir.h
  @ONLY
)

minc_add_executable(minc_unit_tests
  SOURCES
    support/span/span_test.cc
    support/line/line_table_test.cc
    support/source/source_test.cc
    support/diag/diag_test.cc
    support/mem/arena_test.cc
    support/expected/expected_test.cc
    support/intern/interner_test.cc
    support/session/session_test.cc
    support/term/terminal_test.cc
    support/utf8/utf8_test.cc
    lex/lexer_test.cc
    lex/header_name_test.cc
    lex/token_stream_test.cc
    lex/token_dump_test.cc
    lex/lex_report_test.cc
    lex/examples_test.cc
    pp/macro_test.cc
    pp/directives_test.cc
    pp/include_test.cc
    pp/builtins_test.cc
    pp/corpus_test.cc
    pp/differential_test.cc
    parse/syntax_kind_test.cc
    parse/parser_test.cc
    parse/statement_test.cc
    parse/error_codes_test.cc
    parse/examples_parse_test.cc
    syntax/tree_test.cc
    syntax/store_test.cc
    ast/lower_test.cc
    ast/validate_test.cc
    resolve/scope_test.cc
    resolve/errors_test.cc
    resolve/store_test.cc
    sema/type_test.cc
    sema/target_test.cc
    sema/convert_test.cc
    sema/coerce_test.cc
    sema/check_test.cc
    sema/errors_test.cc
    sema/flow_test.cc
    sema/store_test.cc
    sema/examples_test.cc
    driver/cli_test.cc
    driver/error_report_test.cc
    driver/lex_command_test.cc
    driver/parse_command_test.cc
    driver/resolve_command_test.cc
    driver/check_command_test.cc
  DEPS
    minc_support
    minc_lex
    minc_lex_report
    minc_parse
    minc_parse_report
    minc_syntax
    minc_pp
    minc_pp_report
    minc_pp_parse
    minc_ast
    minc_resolve
    minc_resolve_report
    minc_sema
    minc_sema_report
    minc_driver
    GTest::gtest_main
)

# The parse and syntax suites share `parse/parse_fixture.h`, so the test tree
# itself is an include root.
target_include_directories(minc_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

include(GoogleTest)
# PRE_TEST discovery keeps the build step free of test-binary execution, which
# is what breaks cross-compilation and Windows CI the most.
gtest_discover_tests(minc_unit_tests
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
)
