# Test registration helpers (TESTING.md section 1).
#
# Every test carries exactly one label: its family, or `slow`. The test presets `debug`, `asan`,
# `tsan`, ... exclude `slow`; the presets `debug-slow`, `asan-slow`, `tsan-slow` run only `slow`
# tests. (`ctest --preset debug -L slow` cannot work: ctest ANDs a command-line -L with the
# preset's filter; D60.)

include(GoogleTest)

set(DDS_TEST_LABELS
  unit conformance codec property model crash stress repl cli fuzz-replay slow)

# dds_add_gtest(<target> LABEL <label> SOURCES <files...> [LIBS <targets...>]
#               [SLOW_FILTER <gtest pattern>])
# Builds one GoogleTest executable and registers each of its tests with ctest. Tests matching
# SLOW_FILTER are registered with the `slow` label instead of LABEL.
function(dds_add_gtest target)
  cmake_parse_arguments(ARG "" "LABEL;SLOW_FILTER" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_LABEL IN_LIST DDS_TEST_LABELS)
    message(FATAL_ERROR "dds_add_gtest(${target}): unknown label '${ARG_LABEL}'")
  endif()
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE dds_options ${ARG_LIBS} GTest::gtest_main)
  if(ARG_SLOW_FILTER)
    gtest_discover_tests(${target}
      TEST_FILTER "-${ARG_SLOW_FILTER}"
      PROPERTIES LABELS ${ARG_LABEL}
      DISCOVERY_MODE PRE_TEST
      DISCOVERY_TIMEOUT 60)
    gtest_discover_tests(${target}
      TEST_FILTER "${ARG_SLOW_FILTER}"
      PROPERTIES LABELS slow
      DISCOVERY_MODE PRE_TEST
      DISCOVERY_TIMEOUT 60)
  else()
    gtest_discover_tests(${target}
      PROPERTIES LABELS ${ARG_LABEL}
      DISCOVERY_MODE PRE_TEST
      DISCOVERY_TIMEOUT 60)
  endif()
endfunction()
