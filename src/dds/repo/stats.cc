#include "dds/repo/stats.h"

#include <unordered_map>

#include "dds/common/strings.h"
#include "dds/repo/dag_walker.h"
#include "dds/repo/repository.h"

namespace dds {

namespace {

// Reachability over every snapshot, and the logical bytes of all of them.
Status WalkAll(RepoInternals& repo, DagWalker& walker, RepoStats& stats) {
  auto listing = repo.snapshots.List();
  DDS_TRY(listing.status());
  for (const SnapshotId& id : listing->ids) {
    auto manifest = repo.snapshots.Read(id);
    if (manifest.status().code() == StatusCode::kNotFound) continue;  // removed since listing
    DDS_TRY(manifest.status());
    std::vector<std::string> problems;
    walker.Walk(manifest->root, problems);
    if (!problems.empty()) {
      return Status::Corruption(StrCat("snapshot ", SnapshotIdToHex(id), ": ", problems.front(),
                                       " (run `dds verify` for the full list)"));
    }
    ++stats.snapshots;
    stats.logical_bytes += manifest->logical_bytes;
  }
  return Status::Ok();
}

}  // namespace

Result<RepoStats> ComputeStats(RepoInternals& repo) {
  RepoStats stats;
  DagWalker walker(repo, /*deep=*/false);
  DDS_TRY(WalkAll(repo, walker, stats));

  // From the recipes, not the index: the index holds stored lengths, and dedup is measured on
  // content, apart from compression (D40, D107).
  stats.unique_data_bytes = walker.data_content_bytes();
  stats.stored_data_bytes = walker.data_stored_bytes();
  const ContainerSet::Totals totals = repo.containers.PublishedTotals();
  stats.physical_bytes = totals.file_bytes;
  stats.containers = totals.containers;
  stats.blobs = repo.index.Size();
  stats.duplicate_copies = totals.entries - stats.blobs;  // D32
  stats.index_memory_bytes = repo.index.MemoryBytes();
  {
    MutexLock lock(repo.gc.mu);
    stats.gc = repo.gc.totals;
  }
  if (stats.unique_data_bytes > 0) {
    stats.dedup_ratio =
        static_cast<double>(stats.logical_bytes) / static_cast<double>(stats.unique_data_bytes);
    stats.compression_ratio =
        static_cast<double>(stats.unique_data_bytes) / static_cast<double>(stats.stored_data_bytes);
  }

  // Live payload per container: reachable blobs at their canonical (indexed) location, so a
  // duplicate copy counts as dead, as GC's plan treats it (ARCHITECTURE.md 4.3 step 3).
  std::unordered_map<uint32_t, uint64_t> live;
  for (const auto& [fp, loc] : walker.reachable()) live[loc.container_id] += loc.length;
  for (uint32_t id : repo.containers.PublishedNotRetired()) {
    const auto info = repo.containers.Info(id);
    if (!info.has_value() || info->payload_bytes == 0) continue;
    // Integer tenths: live * 10 / payload, so a fraction of exactly 1.0 lands in bucket 10 -> 9.
    const uint64_t tenths = live[id] * 10 / info->payload_bytes;
    ++stats.live_fraction_histogram[std::min<uint64_t>(tenths, 9)];
  }
  return stats;
}

}  // namespace dds
