#pragma once

#include <cstddef>
#include <random>

#include "dds/snapshot/mem_node.h"

namespace dds::testing {

struct TreeGenOptions {
  // Names of arbitrary bytes (except '/' and NUL). Only for trees that stay in memory: real file
  // systems reject some bytes, and macOS folds case (TESTING.md 3.5).
  bool arbitrary_names = true;
  size_t files = 30;                 // files in a new tree
  size_t max_file_size = 512 << 10;  // the occasional large file is this big at most
  // Half the shared pool and half the fresh content drawn from four letters, which zstd shrinks
  // about fourfold, so a compressed repository stores some blobs compressed and some as they are
  // (D107). Off, every byte is uniformly random.
  bool compressible = false;
};

// Seeded random trees for model and stress tests (TESTING.md 3.5): files from 0 bytes up to
// `max_file_size`, including the chunker's boundary sizes at the default parameters; content shared
// across files and across versions; empty and nested directories; symlinks; non-ASCII names.
class TreeGenerator {
 public:
  TreeGenerator(uint64_t seed, TreeGenOptions options);

  // A new tree: `files` random files, plus the fixed edge cases (see the .cc).
  MemTree NewTree();
  // A plausible next version: edits inside files, appends, truncations, copies, deletions, new
  // files, directories and symlinks, and metadata changes. Usually changes something, but may
  // not (a truncation at the end, say), so callers must not assume the tree differs.
  void Mutate(MemTree& tree);

 private:
  std::vector<std::byte> Content();
  std::string Name();
  std::string RandomDir(const MemTree& tree);
  std::string FreshPath(const MemTree& tree, const std::string& dir);
  void AddFile(MemTree& tree);
  void AddDir(MemTree& tree);
  void AddSymlink(MemTree& tree);
  void EditFile(MemNode& file);
  void RemoveSubtree(MemTree& tree, const std::string& path);
  size_t Below(size_t n) { return static_cast<size_t>(rng_() % n); }

  std::mt19937_64 rng_;
  TreeGenOptions options_;
  std::vector<std::vector<std::byte>> pool_;  // shared content: files reuse slices of these
};

}  // namespace dds::testing
