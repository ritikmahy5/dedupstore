#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dds/cli/args.h"
#include "dds/common/process_identity.h"
#include "dds/fs/fs.h"
#include "dds/repo/repository.h"

namespace dds::bench {

// A repository the driver created: on MemFs (`mem` storage mode: CPU cost only, BENCHMARKS.md
// section 1) or on the real file system under `dir` (`disk` mode). Owns its Fs.
struct BenchRepo {
  std::unique_ptr<Fs> fs;
  std::string path;
  std::unique_ptr<Repository> repo;
};

// Creates (removing anything at `dir` first, for disk) and opens a repository.
Result<BenchRepo> FreshRepo(const std::string& storage, const std::string& dir,
                            const RepoParams& params);
// Opens an existing repository on disk.
Result<BenchRepo> OpenDiskRepo(const std::string& dir, const OpenOptions& options = {});

// Unique bytes in the index by blob type.
struct IndexTotals {
  uint64_t data = 0, tree = 0, recipe = 0, blobs = 0;
};
IndexTotals CountIndex(Repository& repo);

double SecondsSince(std::chrono::steady_clock::time_point start);

// Parses `words` against `flags`; on error prints usage and returns nullopt.
std::optional<cli::ParsedArgs> ParseFlags(const std::vector<std::string>& words,
                                          std::vector<cli::FlagSpec> flags);
std::vector<std::string> SplitComma(const std::string& s);
uint64_t FlagU64(const cli::ParsedArgs& a, const std::string& name, uint64_t fallback);
double FlagDouble(const cli::ParsedArgs& a, const std::string& name, double fallback);

// The subcommands; each returns a process exit code.
int RunIngest(const std::vector<std::string>& words);
int RunRestore(const std::vector<std::string>& words);
int RunGc(const std::vector<std::string>& words);
int RunInterference(const std::vector<std::string>& words);
int RunReplicate(const std::vector<std::string>& words);
int RunOpen(const std::vector<std::string>& words);
int RunCheckpoint(const std::vector<std::string>& words);

}  // namespace dds::bench
