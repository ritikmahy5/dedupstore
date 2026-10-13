cmake_minimum_required(VERSION 3.25)

project(dedupstore
  VERSION 0.1.0
  DESCRIPTION "Deduplicating, crash-safe snapshot store"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options. The presets in CMakePresets.json set these; see TESTING.md section 2.
option(DDS_ENABLE_TEST_HOOKS "Compile DDS_MUTATION and DDS_SYNC_POINT hooks (D35)" OFF)
option(DDS_ENABLE_LOCK_ORDER_CHECK "Abort on an out-of-order OrderedMutex acquisition (D59)" OFF)
option(DDS_WERROR "Treat compiler warnings as errors (CI)" OFF)
option(DDS_CLANG_TIDY "Run clang-tidy on the dds sources while compiling" OFF)
option(DDS_FUZZ "Build the fuzz targets as libFuzzer binaries (clang only)" OFF)
option(DDS_BUILD_TESTS "Build the test suites" ON)
option(DDS_BUILD_BENCHMARKS "Build the microbenchmarks and benchmark tools" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DdsDependencies)
include(DdsCompilerOptions)

add_subdirectory(src)

# Always on: the fuzz preset registers its libFuzzer runs with ctest even without the test suites.
enable_testing()

if(DDS_BUILD_TESTS)
  include(DdsTesting)
  add_subdirectory(tests)
endif()

add_subdirectory(fuzz)

if(DDS_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
