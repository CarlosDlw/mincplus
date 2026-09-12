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
    support/utf8/utf8_test.cc
    driver/cli_test.cc
  DEPS
    minc_support
    minc_driver
    GTest::gtest_main
)

include(GoogleTest)
# PRE_TEST discovery keeps the build step free of test-binary execution, which
# is what breaks cross-compilation and Windows CI the most.
gtest_discover_tests(minc_unit_tests
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
)
