// Loading containers on open (ARCHITECTURE.md 7.6 steps 5-7), from the index checkpoint where it
// covers them (D104) and from container footers otherwise.

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/test_hooks.h"
#include "dds/common/thread_annotations.h"
#include "dds/repo/index_checkpoint.h"
#include "dds/repo/repository.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"

namespace dds {

namespace {

constexpr size_t kMaxOpenThreads = 16;

// Shared by the loading threads.
struct LoadResults {
  OrderedMutex mu{lock_level::kLeaf, "open.results"};
  std::vector<std::string> bad DDS_GUARDED_BY(mu);
  // Relaxed: the counters are only read after every loading thread has been joined, and join
  // orders those reads after all the increments.
  std::atomic<uint64_t> valid{0};
  std::atomic<uint64_t> duplicates{0};
};

void LoadOne(RepoInternals& in, uint32_t id, LoadResults& results) {
  auto meta = ReadContainerMetadata(in.fs, in.containers.PathFor(id), id, in.config.repo_id,
                                    in.config.chunker.max_size);
  if (!meta.ok()) {
    // Skipped and reported: verify then names any snapshot that needed it (7.6 step 5).
    MutexLock lock(results.mu);
    results.bad.push_back(meta.status().ToString());
    return;
  }
  uint64_t duplicates = 0;
  for (const SealedEntry& e : meta->entries) duplicates += in.index.RebuildInsert(e) ? 1 : 0;
  in.containers.MarkPublished(
      id, {meta->payload_bytes, meta->file_size, static_cast<uint32_t>(meta->entries.size())});
  results.duplicates.fetch_add(duplicates, std::memory_order_relaxed);
  results.valid.fetch_add(1, std::memory_order_relaxed);
}

struct ContainerListing {
  std::vector<uint32_t> ids;  // increasing
  uint32_t max_id = 0;
  std::vector<std::string> ignored;
};

Result<ContainerListing> ListContainers(Fs& fs, const RepoLayout& layout) {
  auto names = fs.ListDir(layout.containers());
  DDS_TRY(names.status());
  ContainerListing listing;
  for (const std::string& name : *names) {
    if (auto id = ParseContainerFileName(name)) {
      listing.ids.push_back(*id);
    } else {
      listing.ignored.push_back(name);
    }
  }
  std::sort(listing.ids.begin(), listing.ids.end());
  if (!listing.ids.empty()) listing.max_id = listing.ids.back();
  // 7.6 step 6: new ids go above every container name, including ones that fail to load, so a
  // damaged container's file is never overwritten by a new one.
  DDS_CHECK(listing.max_id < std::numeric_limits<uint32_t>::max());
  return listing;
}

// 7.6 step 7: the footers of `ids`, in parallel. RebuildInsert gives the same index in any order.
void LoadFooters(RepoInternals& in, const std::vector<uint32_t>& ids, size_t threads,
                 OpenReport& report) {
  if (threads == 0) threads = std::max<size_t>(1, std::thread::hardware_concurrency());
  threads = std::min({threads, kMaxOpenThreads, std::max<size_t>(1, ids.size())});
  LoadResults results;
  auto work = [&](size_t first) {
    for (size_t i = first; i < ids.size(); i += threads) LoadOne(in, ids[i], results);
  };
  std::vector<std::thread> pool;
  for (size_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
  work(0);
  for (std::thread& t : pool) t.join();
  {
    MutexLock lock(results.mu);
    report.bad_containers = std::move(results.bad);
  }
  std::sort(report.bad_containers.begin(), report.bad_containers.end());
  report.footers_read = ids.size();
  report.containers += results.valid.load(std::memory_order_relaxed);
  report.duplicate_copies += results.duplicates.load(std::memory_order_relaxed);
}

// Listed ids the checkpoint did not cover (both increasing).
std::vector<uint32_t> NotCovered(const std::vector<uint32_t>& listed,
                                 const std::vector<uint32_t>& covered) {
  std::vector<uint32_t> out;
  std::set_difference(listed.begin(), listed.end(), covered.begin(), covered.end(),
                      std::back_inserter(out));
  return out;
}

// The checkpoint's part of open, if a checkpoint exists. With `options.use_index_checkpoint`, its
// entries go into out.internals and the containers it covered drop out of `footer_ids`. Either
// way, `next_id` rises to the checkpoint's, so no container id it covers is ever reused, which is
// what lets open trust a covered id without reading that container. A checkpoint that fails any
// check is removed, so it is never read again, and the load starts over from every footer, since
// the failed load may have inserted part of it.
Status ApplyCheckpoint(Fs& fs, const RepoLayout& layout, const RepoConfig& config,
                       const Clock& clock, const OpenOptions& options,
                       const std::vector<uint32_t>& listed, uint32_t& next_id,
                       std::vector<uint32_t>& footer_ids, LoadedRepository& out) {
  auto exists = fs.Exists(layout.index_checkpoint());
  DDS_TRY(exists.status());
  if (!*exists) return Status::Ok();
  if (!options.use_index_checkpoint) {
    auto checkpoint_next = ReadCheckpointNextId(fs, layout, config.repo_id);
    if (checkpoint_next.ok()) {
      next_id = std::max(next_id, *checkpoint_next);
      return Status::Ok();
    }
    out.report.checkpoint_rejected = checkpoint_next.status().ToString();
    return RemoveIndexCheckpoint(fs, layout);
  }
  auto loaded = LoadIndexCheckpoint(*out.internals, listed, options.open_threads);
  if (!loaded.ok()) {
    out.report.checkpoint_rejected = loaded.status().ToString();
    out.internals = std::make_unique<RepoInternals>(fs, layout, config, clock);
    return RemoveIndexCheckpoint(fs, layout);
  }
  // Broken on purpose under the mutation (IndexCheckpointTest): ids the checkpoint covers could
  // then be reused.
  if (!DDS_MUTATION("checkpoint_next_id_ignored")) {
    next_id = std::max(next_id, loaded->next_container_id);
  }
  footer_ids = NotCovered(listed, loaded->used);
  out.report.checkpoint_containers = loaded->used.size();
  out.report.containers = loaded->used.size();
  out.checkpoint_covered = std::move(loaded->used);
  return Status::Ok();
}

}  // namespace

Result<OpenReport> LoadContainers(RepoInternals& in, size_t threads) {
  auto listing = ListContainers(in.fs, in.layout);
  DDS_TRY(listing.status());
  OpenReport report;
  report.ignored_files = std::move(listing->ignored);
  in.containers.SetNextId(listing->max_id + 1);
  LoadFooters(in, listing->ids, threads, report);
  report.blobs = in.index.Size();
  return report;
}

Result<LoadedRepository> LoadRepository(Fs& fs, const RepoLayout& layout, const RepoConfig& config,
                                        const Clock& clock, const OpenOptions& options) {
  auto listing = ListContainers(fs, layout);
  DDS_TRY(listing.status());
  LoadedRepository out;
  out.internals = std::make_unique<RepoInternals>(fs, layout, config, clock);
  out.report.ignored_files = std::move(listing->ignored);
  uint32_t next_id = listing->max_id + 1;
  std::vector<uint32_t> footer_ids = listing->ids;
  DDS_TRY(
      ApplyCheckpoint(fs, layout, config, clock, options, listing->ids, next_id, footer_ids, out));
  out.internals->containers.SetNextId(next_id);
  LoadFooters(*out.internals, footer_ids, options.open_threads, out.report);
  out.report.blobs = out.internals->index.Size();
  return out;
}

}  // namespace dds
