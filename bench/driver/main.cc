// dds_bench: the benchmark driver (BENCHMARKS.md). Each subcommand runs one kind of measurement
// --warmup times (discarded) and then --runs times, and writes the driver part of a result file
// (report.h) to --out. The SLURM job scripts in bench/slurm/ call it, and tools/finalize_result.py
// turns its output into bench/results/<experiment>/<run_id>.json.
//
//   dds_bench ingest       --experiment E1 --versions DIR,DIR,... [--storage mem|disk] ...
//   dds_bench restore      --experiment E4 --repo DIR --snapshot ID --dest DIR [--verify 0|1]
//   dds_bench gc           --experiment E5 --repo DIR --work DIR --threshold F
//   dds_bench interference --experiment E6 --versions DIR,... --mode sequential|with-gc|concurrent
//   dds_bench replicate    --experiment E7 --base DIR --next DIR --latency-ms L --mbit B --window W
//   dds_bench open         --experiment E9 --repo DIR

#include <cstdio>
#include <string>
#include <vector>

#include "driver/driver.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: dds_bench ingest|restore|gc|interference|replicate|open|checkpoint ...\n");
    return 1;
  }
  const std::string command = argv[1];
  const std::vector<std::string> words(argv + 2, argv + argc);
  if (command == "ingest") return dds::bench::RunIngest(words);
  if (command == "restore") return dds::bench::RunRestore(words);
  if (command == "gc") return dds::bench::RunGc(words);
  if (command == "interference") return dds::bench::RunInterference(words);
  if (command == "replicate") return dds::bench::RunReplicate(words);
  if (command == "open") return dds::bench::RunOpen(words);
  if (command == "checkpoint") return dds::bench::RunCheckpoint(words);
  std::fprintf(stderr, "dds_bench: unknown command '%s'\n", command.c_str());
  return 1;
}
