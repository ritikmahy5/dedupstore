# Compiler options shared by every dds target (never applied to third-party code).
#
# dds_options is an INTERFACE library: linking it gives a target the project's warning flags,
# the build-flag definitions, and, with clang, the thread-safety analysis (D17).

add_library(dds_options INTERFACE)

set(DDS_IS_CLANG FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|IntelLLVM")
  set(DDS_IS_CLANG TRUE)
endif()

target_compile_options(dds_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(DDS_IS_CLANG)
  # Every clang build checks the DDS_GUARDED_BY / DDS_REQUIRES annotations, not only CI's.
  target_compile_options(dds_options INTERFACE -Wthread-safety)
endif()
if(DDS_WERROR)
  target_compile_options(dds_options INTERFACE -Werror)
endif()

if(DDS_ENABLE_TEST_HOOKS)
  target_compile_definitions(dds_options INTERFACE DDS_ENABLE_TEST_HOOKS)
endif()
if(DDS_ENABLE_LOCK_ORDER_CHECK)
  target_compile_definitions(dds_options INTERFACE DDS_ENABLE_LOCK_ORDER_CHECK)
endif()

target_include_directories(dds_options INTERFACE "${PROJECT_SOURCE_DIR}/src")

find_package(Threads REQUIRED)
target_link_libraries(dds_options INTERFACE Threads::Threads)

# Apple's linker warns when a static library appears twice on a link line, which CMake does for
# transitive module dependencies. The duplicates are harmless; the warning is noise.
if(APPLE)
  include(CheckLinkerFlag)
  check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" DDS_HAVE_NO_WARN_DUPLICATE_LIBS)
  if(DDS_HAVE_NO_WARN_DUPLICATE_LIBS)
    target_link_options(dds_options INTERFACE "LINKER:-no_warn_duplicate_libraries")
  endif()
endif()

if(DDS_CLANG_TIDY)
  # scripts/dev/setup_tools.sh installs the pinned clang-tidy into .tools/venv (D61).
  find_program(DDS_CLANG_TIDY_EXE NAMES clang-tidy
    HINTS "${PROJECT_SOURCE_DIR}/.tools/venv/bin" REQUIRED)
  message(STATUS "clang-tidy: ${DDS_CLANG_TIDY_EXE}")
  set(DDS_CLANG_TIDY_COMMAND "${DDS_CLANG_TIDY_EXE};--warnings-as-errors=*")
  if(APPLE)
    # CMake 4 no longer passes -isysroot when none is configured. Apple clang finds the SDK on its
    # own; a standalone clang-tidy needs to be told where the SDK's headers are.
    execute_process(COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE DDS_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND DDS_CLANG_TIDY_COMMAND "--extra-arg=-isysroot${DDS_MACOS_SDK}")
  endif()
endif()

if(DDS_FUZZ)
  if(NOT DDS_IS_CLANG)
    message(FATAL_ERROR "DDS_FUZZ needs clang with libFuzzer (TESTING.md section 2, D34)")
  endif()
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles(
    "#include <cstddef>
     #include <cstdint>
     extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }"
    DDS_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT DDS_HAVE_LIBFUZZER)
    message(FATAL_ERROR
      "This compiler does not support -fsanitize=fuzzer (Apple clang does not). Use Homebrew "
      "LLVM or a Linux clang for the fuzz preset (D34).")
  endif()
endif()

# dds_add_library(<name> SOURCES <files...> [DEPS <targets...>])
# A static library for one module. DEPS must only name modules to the module's left in
# ARCHITECTURE.md section 1, which keeps the dependency direction one-way (D53).
function(dds_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${name} STATIC ${ARG_SOURCES})
  target_link_libraries(${name} PUBLIC dds_options ${ARG_DEPS})
  if(DDS_CLANG_TIDY)
    set_target_properties(${name} PROPERTIES CXX_CLANG_TIDY "${DDS_CLANG_TIDY_COMMAND}")
  endif()
endfunction()
