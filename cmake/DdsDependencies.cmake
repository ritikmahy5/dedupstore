# Third-party dependencies (D4): BLAKE3 (C), GoogleTest, Google Benchmark, and zstd (C) for
# compression (D107). Nothing else.
# Each is fetched from a release URL pinned by SHA-256, so a build needs no system packages and
# always compiles the same source (versions: D58).

include(FetchContent)

# Optional directory holding the release tarballs under the file names below, for machines whose
# build nodes cannot reach GitHub (Explorer's compute nodes, D57). The SHA-256 check is the same.
# It is filled beforehand on a machine that can reach GitHub.
set(DDS_DEPS_MIRROR "" CACHE PATH "Directory with dependency tarballs for offline builds")

# Sets <out_var> to where one tarball comes from: the mirror copy if a mirror is set, otherwise the
# release URL. (A list mixing a local file and a URL is rejected by ExternalProject, so there is no
# fallback from one to the other.)
function(dds_dependency_urls out_var file_name url)
  if(DDS_DEPS_MIRROR)
    set(${out_var} "${DDS_DEPS_MIRROR}/${file_name}" PARENT_SCOPE)
  else()
    set(${out_var} "${url}" PARENT_SCOPE)
  endif()
endfunction()

# BLAKE3: the repository's c/ directory ships its own CMakeLists.txt (D4).
dds_dependency_urls(DDS_BLAKE3_URLS blake3-1.8.7.tar.gz
  https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.7.tar.gz)
FetchContent_Declare(blake3
  URL ${DDS_BLAKE3_URLS}
  URL_HASH SHA256=c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c
  SOURCE_SUBDIR c
  SYSTEM)
set(BLAKE3_USE_TBB OFF CACHE BOOL "" FORCE)
set(BLAKE3_FETCH_TBB OFF CACHE BOOL "" FORCE)
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  # BLAKE3's CMakeLists picks SIMD sources only for GNU, Clang and AppleClang. Intel's clang (the
  # Explorer clang, D57) is the same LLVM driver: give it BLAKE3's own options for Clang, using the
  # C intrinsics versions instead of the assembly.
  set(BLAKE3_SIMD_TYPE "x86-intrinsics" CACHE STRING "")
  set(BLAKE3_CFLAGS_SSE2 "-msse2" CACHE STRING "")
  set(BLAKE3_CFLAGS_SSE4.1 "-msse4.1" CACHE STRING "")
  set(BLAKE3_CFLAGS_AVX2 "-mavx2" CACHE STRING "")
  set(BLAKE3_CFLAGS_AVX512 "-mavx512f -mavx512vl" CACHE STRING "")
endif()
FetchContent_MakeAvailable(blake3)
# The official test vectors live outside c/; the BLAKE3 test reads them from the fetched source.
set(DDS_BLAKE3_TEST_VECTORS "${blake3_SOURCE_DIR}/test_vectors/test_vectors.json"
  CACHE INTERNAL "Path to BLAKE3 test_vectors.json")

# zstd: the release tarball's build/cmake directory, the static library only (D107). Legacy
# formats (frames written by zstd before 0.8) and multithreading stay off: every frame here is
# written by this library, one blob at a time.
dds_dependency_urls(DDS_ZSTD_URLS zstd-1.5.7.tar.gz
  https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz)
FetchContent_Declare(zstd
  URL ${DDS_ZSTD_URLS}
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  SOURCE_SUBDIR build/cmake
  SYSTEM)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

if(DDS_BUILD_TESTS)
  dds_dependency_urls(DDS_GOOGLETEST_URLS googletest-1.18.0.tar.gz
    https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz)
  FetchContent_Declare(googletest
    URL ${DDS_GOOGLETEST_URLS}
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(DDS_BUILD_BENCHMARKS)
  dds_dependency_urls(DDS_BENCHMARK_URLS benchmark-1.9.5.tar.gz
    https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz)
  FetchContent_Declare(benchmark
    URL ${DDS_BENCHMARK_URLS}
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()
