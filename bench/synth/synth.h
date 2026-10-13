#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dds/common/status.h"
#include "dds/snapshot/source_tree.h"

namespace dds::bench {

// The `synth` dataset (BENCHMARKS.md section 4, D93). Version 0 is `random_bytes` of seeded random
// data and `text_bytes` of seeded text-like data (words from a fixed pseudo-vocabulary), split into
// files whose sizes are drawn from [min_file, max_file]. Version k applies edits to every file of
// version k-1: inserts, deletes, and overwrites of 1 to 4096 bytes at random offsets, until about
// `edit_rate` of the file's bytes have been touched. All randomness is SplitMix64 keyed by
// (seed, version, file), so any version can be regenerated alone and every run sees the same bytes.
struct SynthOptions {
  uint64_t seed = 1;
  uint64_t random_bytes = uint64_t{2} << 30;
  uint64_t text_bytes = uint64_t{1} << 30;
  uint32_t versions = 10;  // versions 1..versions after the base
  double edit_rate = 0.01;
  uint64_t min_file = uint64_t{1} << 20;
  uint64_t max_file = uint64_t{16} << 20;
};

struct SynthFile {
  std::string name;  // "r0000" (random) or "t0000" (text)
  std::vector<std::byte> content;
};

// Generates version `version` one file at a time (only one file is in memory at once), passing
// each to `emit` in name order. Stops at the first error `emit` returns.
Status GenerateSynth(const SynthOptions& options, uint32_t version,
                     const std::function<Status(SynthFile&& file)>& emit);

// A SourceTree of `files` files of `file_size` seeded random bytes each, produced on demand while
// the backup reads them, so E9 can build repositories of 100 GiB of unique data without writing the
// input anywhere. Safe to read from several threads.
std::unique_ptr<SourceTree> MakeRandomSourceTree(uint64_t seed, uint64_t files, uint64_t file_size);

}  // namespace dds::bench
