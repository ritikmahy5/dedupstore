# Microbenchmarks (BENCHMARKS.md section 6) and the benchmark tools. Build them in the
# `release` preset: only release numbers mean anything, and only Explorer's are reported (D37).

add_executable(dds_micro_bench
  micro/chunker_bench.cc
  micro/hash_bench.cc
  micro/index_bench.cc)
target_link_libraries(dds_micro_bench PRIVATE dds_chunk dds_index benchmark::benchmark_main)

# The synthetic dataset (BENCHMARKS.md section 4) and its generator.
add_library(dds_bench_synth STATIC synth/synth.cc)
target_include_directories(dds_bench_synth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dds_bench_synth PUBLIC dds_snapshot dds_chunk)

add_executable(dds_gen_synth gen_synth.cc)
target_link_libraries(dds_gen_synth PRIVATE dds_bench_synth dds_cli)

# The benchmark driver (BENCHMARKS.md section 5); SLURM jobs in bench/slurm/ run it.
add_executable(dds_bench
  driver/driver.cc
  driver/gc.cc
  driver/ingest.cc
  driver/interference.cc
  driver/main.cc
  driver/open.cc
  driver/replicate.cc
  driver/report.cc
  driver/restore.cc)
target_include_directories(dds_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dds_bench PRIVATE dds_bench_synth dds_cli dds_repl)
