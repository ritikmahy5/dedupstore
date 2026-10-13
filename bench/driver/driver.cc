#include "driver/driver.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "dds/cli/numbers.h"
#include "dds/fs/mem_fs.h"
#include "dds/fs/posix_fs.h"

namespace dds::bench {

namespace {

const PosixProcessIdentity& Self() {
  static const PosixProcessIdentity self;
  return self;
}

}  // namespace

Result<BenchRepo> FreshRepo(const std::string& storage, const std::string& dir,
                            const RepoParams& params) {
  BenchRepo r;
  if (storage == "mem") {
    r.fs = std::make_unique<MemFs>();
    r.path = "/repo";
  } else if (storage == "disk") {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    r.fs = std::make_unique<PosixFs>();
    r.path = dir;
  } else {
    return Status::InvalidArgument("--storage must be mem or disk");
  }
  DDS_TRY(Repository::Init(*r.fs, r.path, params).status());
  auto opened = Repository::Open(*r.fs, r.path, Self());
  DDS_TRY(opened.status());
  r.repo = std::move(opened).value();
  return r;
}

Result<BenchRepo> OpenDiskRepo(const std::string& dir, const OpenOptions& options) {
  BenchRepo r;
  r.fs = std::make_unique<PosixFs>();
  r.path = dir;
  auto opened = Repository::Open(*r.fs, r.path, Self(), options);
  DDS_TRY(opened.status());
  r.repo = std::move(opened).value();
  return r;
}

IndexTotals CountIndex(Repository& repo) {
  IndexTotals t;
  ChunkIndex& index = repo.Internals().index;
  for (size_t shard = 0; shard < ChunkIndex::kShardCount; ++shard) {
    index.ForEachInShard(shard, [&](const IndexEntry& e) {
      ++t.blobs;
      if (e.type == BlobType::kData) t.data += e.length;
      if (e.type == BlobType::kTree) t.tree += e.length;
      if (e.type == BlobType::kRecipe) t.recipe += e.length;
    });
  }
  return t;
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::optional<cli::ParsedArgs> ParseFlags(const std::vector<std::string>& words,
                                          std::vector<cli::FlagSpec> flags) {
  flags.push_back({"out", true});
  flags.push_back({"runs", true});
  flags.push_back({"warmup", true});
  flags.push_back({"experiment", true});
  std::string error;
  auto parsed = cli::ParseArgs(words, flags, &error);
  if (!parsed) std::fprintf(stderr, "dds_bench: %s\n", error.c_str());
  return parsed;
}

std::vector<std::string> SplitComma(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t comma = s.find(',', start);
    const std::string part =
        s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!part.empty()) out.push_back(part);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

uint64_t FlagU64(const cli::ParsedArgs& a, const std::string& name, uint64_t fallback) {
  auto t = a.Get(name);
  if (!t) return fallback;
  auto v = cli::ParseSize(*t);  // plain numbers, or K/M suffixes
  if (!v) {
    std::fprintf(stderr, "dds_bench: bad --%s '%s'\n", name.c_str(), t->c_str());
    std::exit(1);
  }
  return *v;
}

double FlagDouble(const cli::ParsedArgs& a, const std::string& name, double fallback) {
  auto t = a.Get(name);
  return t ? std::strtod(t->c_str(), nullptr) : fallback;
}

}  // namespace dds::bench
