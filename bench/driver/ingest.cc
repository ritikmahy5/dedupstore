// dds_bench ingest: back up a sequence of dataset versions into a fresh repository, per sample
// (E1, E2, E3, E8; and building the repositories E4, E5, and E9 use).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <unordered_set>

#include "dds/snapshot/recipe.h"
#include "dds/snapshot/source_tree.h"
#include "dds/snapshot/tree.h"
#include "driver/driver.h"
#include "driver/report.h"
#include "synth/synth.h"

namespace dds::bench {

namespace {

constexpr const char* kUsage =
    "dds_bench ingest --experiment E --out FILE (--versions DIR,... | --synthetic-files N "
    "--synthetic-file-size B) [--labels L,...] [--dataset NAME] [--storage mem|disk] [--repo DIR] "
    "[--chunker fastcdc|fixed] [--avg B] [--min B] [--max B] [--container-size B] [--threads N] "
    "[--per-file 1] [--keep 1] [--parent previous|none] [--compression none|zstd[:L]] "
    "[--split-mib N] [--runs N] [--warmup N]";

// For E8's "dedup by file": the bytes of each file, and of its chunks no earlier version had.
class PerFileCounter {
 public:
  void Count(Repository& repo, const SnapshotId& id, const std::string& label,
             std::map<std::string, double>& counters) {
    RepoInternals& in = repo.Internals();
    std::vector<Fingerprint> trees = {in.snapshots.Read(id)->root};
    std::vector<Fingerprint> this_version;
    while (!trees.empty()) {
      const Fingerprint fp = trees.back();
      trees.pop_back();
      auto tree = DecodeTree(*in.containers.Read(*in.index.Lookup(fp)));
      for (const TreeEntry& e : tree->entries) {
        if (e.kind == EntryKind::kDirectory) trees.push_back(e.subtree);
        if (e.kind != EntryKind::kFile) continue;
        double fresh = 0;
        for (const Fingerprint& piece : e.pieces) {
          auto recipe = DecodeRecipe(*in.containers.Read(*in.index.Lookup(piece)),
                                     in.config.chunker.max_size);
          for (const ChunkRef& ref : recipe->refs) {
            if (seen_.count(ref.fp) == 0) fresh += ref.length;
            this_version.push_back(ref.fp);
          }
        }
        counters[label + ".file." + e.name + ".bytes"] += static_cast<double>(e.size);
        counters[label + ".file." + e.name + ".new_bytes"] += fresh;
      }
    }
    seen_.insert(this_version.begin(), this_version.end());
  }

 private:
  std::unordered_set<Fingerprint, FingerprintHash> seen_;  // chunks of earlier versions
};

void AddTimings(const std::string& label, const BackupTimings& t,
                std::map<std::string, double>& c) {
  c[label + ".t.walk"] = static_cast<double>(t.walk_ns) / 1e9;
  c[label + ".t.read_chunk"] = static_cast<double>(t.read_chunk_ns) / 1e9;
  c[label + ".t.hash"] = static_cast<double>(t.hash_ns) / 1e9;
  c[label + ".t.store"] = static_cast<double>(t.store_ns) / 1e9;
  c[label + ".t.reuse"] = static_cast<double>(t.reuse_ns) / 1e9;
  c[label + ".t.trees"] = static_cast<double>(t.trees_ns) / 1e9;
  c[label + ".t.commit"] = static_cast<double>(t.commit_ns) / 1e9;
}

void AddFinal(Repository& repo, double logical_total, std::map<std::string, double>& c) {
  const IndexTotals t = CountIndex(repo);
  RepoInternals& in = repo.Internals();
  const ContainerSet::Totals ct = in.containers.PublishedTotals();
  // The index holds stored lengths. Without compression they are the content's; with it
  // (D107) the content bytes come from the recipes, through stats, so the dedup ratio stays a
  // ratio of content and compression is reported apart from it.
  double unique = static_cast<double>(t.data);
  c["final.stored_data_bytes"] = static_cast<double>(t.data);
  if (in.config.compression.codec != Codec::kNone) {
    auto stats = repo.Stats();
    if (stats.ok()) unique = static_cast<double>(stats->unique_data_bytes);
    c["final.compression_ratio"] = t.data == 0 ? 0 : unique / static_cast<double>(t.data);
  }
  c["final.blobs"] = static_cast<double>(t.blobs);
  c["final.unique_data_bytes"] = unique;
  c["final.index_bytes"] = static_cast<double>(in.index.MemoryBytes());
  c["final.index_bytes_per_blob"] =
      t.blobs == 0 ? 0 : static_cast<double>(in.index.MemoryBytes()) / static_cast<double>(t.blobs);
  c["final.containers"] = static_cast<double>(ct.containers);
  c["final.physical_bytes"] = static_cast<double>(ct.file_bytes);
  c["final.dedup_ratio"] = unique == 0 ? 0 : logical_total / unique;
  // Data blob sizes in power-of-two buckets: hist.le_<2^k> counts blobs of at most 2^k bytes.
  std::map<int, double> hist;
  for (size_t shard = 0; shard < ChunkIndex::kShardCount; ++shard) {
    in.index.ForEachInShard(shard, [&](const IndexEntry& e) {
      if (e.type == BlobType::kData) ++hist[static_cast<int>(std::ceil(std::log2(e.length)))];
    });
  }
  for (const auto& [k, n] : hist) c["hist.le_" + std::to_string(uint64_t{1} << k)] = n;
}

}  // namespace

int RunIngest(const std::vector<std::string>& words) {
  auto a = ParseFlags(words, {{"versions", true},
                              {"labels", true},
                              {"dataset", true},
                              {"synthetic-files", true},
                              {"synthetic-file-size", true},
                              {"storage", true},
                              {"repo", true},
                              {"chunker", true},
                              {"avg", true},
                              {"min", true},
                              {"max", true},
                              {"container-size", true},
                              {"threads", true},
                              {"per-file", true},
                              {"keep", true},
                              {"parent", true},
                              {"compression", true},
                              {"split-mib", true},
                              {"synthetic-seed", true}});
  if (!a || !a->Get("out") || !a->Get("experiment")) {
    std::fprintf(stderr, "usage: %s\n", kUsage);
    return 1;
  }
  const std::vector<std::string> dirs = SplitComma(a->Get("versions").value_or(""));
  const uint64_t synthetic_files = FlagU64(*a, "synthetic-files", 0);
  if (dirs.empty() == (synthetic_files == 0)) {
    std::fprintf(stderr, "dds_bench ingest: give --versions or --synthetic-files\n");
    return 1;
  }
  std::vector<std::string> labels = SplitComma(a->Get("labels").value_or(""));
  const size_t versions = dirs.empty() ? 1 : dirs.size();
  for (size_t i = labels.size(); i < versions; ++i) labels.push_back("v" + std::to_string(i));
  const std::string storage = a->Get("storage").value_or("mem");
  RepoParams params;
  const std::string chunker = a->Get("chunker").value_or("fastcdc");
  const uint64_t avg = FlagU64(*a, "avg", 8192);
  params.chunker.kind = chunker == "fixed" ? ChunkerKind::kFixed : ChunkerKind::kFastCdc;
  params.chunker.avg_size = static_cast<uint32_t>(avg);
  params.chunker.min_size =
      static_cast<uint32_t>(chunker == "fixed" ? avg : FlagU64(*a, "min", avg / 4));
  params.chunker.max_size =
      static_cast<uint32_t>(chunker == "fixed" ? avg : FlagU64(*a, "max", avg * 8));
  if (chunker == "fixed") params.chunker.normalization = 0;
  params.container_target =
      static_cast<uint32_t>(FlagU64(*a, "container-size", kDefaultContainerTarget));
  auto compression = ParseCompression(a->Get("compression").value_or("none"));
  if (!compression.ok()) {
    std::fprintf(stderr, "dds_bench ingest: %s\n", compression.status().ToString().c_str());
    return 1;
  }
  params.compression = *compression;
  const size_t threads = static_cast<size_t>(FlagU64(*a, "threads", 1));
  // Files of at least this many MiB are split across workers (D109); 0, the default, never
  // splits, so experiments from before splitting existed rerun as they ran.
  const uint64_t split_mib = FlagU64(*a, "split-mib", 0);
  const uint64_t split_bytes = split_mib == 0 ? UINT64_MAX : split_mib << 20;
  const bool per_file = FlagU64(*a, "per-file", 0) != 0;
  // Parent snapshots (D106): with "previous", each version after the first is backed up with the
  // one before as its parent.
  const std::string parent = a->Get("parent").value_or("none");
  if (parent != "none" && parent != "previous") {
    std::fprintf(stderr, "dds_bench ingest: --parent must be previous or none\n");
    return 1;
  }
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);

  Report report(*a->Get("experiment"));
  report.Param("storage", storage);
  report.Param("chunker", chunker == "fixed" ? "fixed" : "fastcdc-2016");
  report.Param("min", static_cast<double>(params.chunker.min_size));
  report.Param("avg", static_cast<double>(params.chunker.avg_size));
  report.Param("max", static_cast<double>(params.chunker.max_size));
  report.Param("container_target", static_cast<double>(params.container_target));
  report.Param("threads", static_cast<double>(threads));
  report.Param("parent", parent);
  report.Param("compression", CompressionText(params.compression));
  report.Param("split_mib", static_cast<double>(split_mib));
  report.Dataset("name", a->Get("dataset").value_or(dirs.empty() ? "synthetic-random" : "unnamed"));
  std::string joined;
  for (const std::string& l : labels) joined += (joined.empty() ? "" : ",") + l;
  report.Dataset("versions", joined);

  for (uint64_t run = 0; run < warmup + runs; ++run) {
    auto r = FreshRepo(storage, a->Get("repo").value_or("/tmp/dds-bench-repo"), params);
    if (!r.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", r.status().ToString().c_str());
      return 1;
    }
    std::map<std::string, double> c;
    PerFileCounter per_file_counter;
    double logical_total = 0, wall_total = 0;
    IndexTotals before = CountIndex(*r->repo);
    std::optional<SnapshotId> previous;
    for (size_t v = 0; v < versions; ++v) {
      std::unique_ptr<SourceTree> source =
          dirs.empty() ? MakeRandomSourceTree(FlagU64(*a, "synthetic-seed", 1), synthetic_files,
                                              FlagU64(*a, "synthetic-file-size", 64 << 20))
                       : std::make_unique<PosixSourceTree>(dirs[v]);
      BackupOptions options;
      options.threads = threads;
      options.split_file_bytes = split_bytes;
      if (parent == "previous") options.parent = previous;
      const auto start = std::chrono::steady_clock::now();
      auto b = r->repo->Backup(*source, options);
      const double seconds = SecondsSince(start);
      if (!b.ok()) {
        std::fprintf(stderr, "dds_bench: backup %s: %s\n", labels[v].c_str(),
                     b.status().ToString().c_str());
        return 1;
      }
      const IndexTotals after = CountIndex(*r->repo);
      const std::string& l = labels[v];
      const double logical = static_cast<double>(b->manifest.logical_bytes);
      logical_total += logical;
      wall_total += seconds;
      c[l + ".seconds"] = seconds;
      c[l + ".logical_bytes"] = logical;
      c[l + ".files"] = static_cast<double>(b->manifest.file_count);
      c[l + ".mb_per_s"] = logical / 1e6 / seconds;
      c[l + ".new_data_bytes"] = static_cast<double>(after.data - before.data);
      c[l + ".new_tree_bytes"] = static_cast<double>(after.tree - before.tree);
      c[l + ".new_recipe_bytes"] = static_cast<double>(after.recipe - before.recipe);
      c[l + ".dedup_ratio"] = after.data == 0 ? 0 : logical_total / static_cast<double>(after.data);
      c[l + ".files_reused"] = static_cast<double>(b->files_reused);
      c[l + ".bytes_reused"] = static_cast<double>(b->bytes_reused);
      c[l + ".files_split"] = static_cast<double>(b->files_split);
      c[l + ".segments_queued"] = static_cast<double>(b->segments_queued);
      // The root's first 48 bits, exact in a double: equal roots mean equal snapshots (S5ab).
      uint64_t root48 = 0;
      for (size_t i = 0; i < 6; ++i) root48 = (root48 << 8) | b->manifest.root.bytes[i];
      c[l + ".root48"] = static_cast<double>(root48);
      previous = b->id;
      AddTimings(l, b->timings, c);
      if (per_file) per_file_counter.Count(*r->repo, b->id, l, c);
      before = after;
    }
    c["total.seconds"] = wall_total;
    c["total.logical_bytes"] = logical_total;
    c["total.mb_per_s"] = logical_total / 1e6 / wall_total;
    AddFinal(*r->repo, logical_total, c);
    if (run >= warmup) report.AddSample(wall_total, std::move(c));
    if (storage == "disk" && !(FlagU64(*a, "keep", 0) != 0 && run + 1 == warmup + runs)) {
      std::error_code ec;
      r->repo.reset();
      std::filesystem::remove_all(r->path, ec);
    }
  }
  return report.Write(*a->Get("out")) ? 0 : 1;
}

}  // namespace dds::bench
