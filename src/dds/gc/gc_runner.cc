#include "dds/gc/gc_runner.h"

#include "dds/common/strings.h"
#include "dds/common/test_hooks.h"
#include "dds/gc/planner.h"
#include "dds/repo/repository.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"

namespace dds {

namespace {

double SecondsSince(const Clock& clock, int64_t start) {
  return static_cast<double>(clock.MonotonicNanos() - start) / 1e9;
}

// The candidate set (4.3 step 1): published, not retired containers. A container sealed but not
// yet published is not a candidate (6.2 item 5, D49).
Result<std::vector<Candidate>> Candidates(RepoInternals& repo) {
  std::vector<Candidate> out;
  if (DDS_MUTATION("candidate_before_publish")) {
    // Broken on purpose (TESTING.md 3.7): every container file on disk, published or not.
    auto names = repo.fs.ListDir(repo.layout.containers());
    DDS_TRY(names.status());
    for (const std::string& name : *names) {
      const auto id = ParseContainerFileName(name);
      if (!id.has_value()) continue;
      auto meta = ReadContainerMetadata(repo.fs, repo.containers.PathFor(*id), *id,
                                        repo.config.repo_id, repo.config.chunker.max_size);
      DDS_TRY(meta.status());
      out.push_back({*id, meta->payload_bytes, meta->file_size});
    }
    return out;
  }
  for (uint32_t id : repo.containers.PublishedNotRetired()) {
    const auto info = repo.containers.Info(id);
    if (info.has_value()) out.push_back({id, info->payload_bytes, info->file_bytes});
  }
  return out;
}

void Summarize(const std::vector<PlannedContainer>& plan, GcReport& report) {
  for (const PlannedContainer& p : plan) {
    report.candidate_payload_bytes += p.candidate.payload_bytes;
    report.live_bytes += p.live_bytes;
    switch (p.action) {
      case PlanAction::kDelete:
        ++report.planned_delete;
        break;
      case PlanAction::kCompact:
        ++report.planned_compact;
        break;
      case PlanAction::kKeep:
        ++report.planned_keep;
        break;
    }
  }
}

}  // namespace

GcRunner::GcRunner(Repository& repo) : repo_(repo.Internals()) {}

Result<GcReport> GcRunner::Run(const GcOptions& options) {
  if (!(options.threshold > 0 && options.threshold <= 1)) {
    return Status::InvalidArgument(
        StrCat("gc threshold must be in (0, 1], got ", options.threshold));
  }
  const Clock& clock = repo_.clock;
  GcReport report;
  report.dry_run = options.dry_run;
  MutexLock gc_lock(repo_.gc.mu);  // one GC at a time; the lowest lock level, held for the run
  if (!options.dry_run) {
    // Containers a previous run retired while readers were active.
    auto leftover = repo_.containers.ReclaimRetired(repo_.epochs.OldestReader());
    DDS_TRY(leftover.status());
    report.containers_reclaimed += leftover->containers;
    report.bytes_reclaimed += leftover->file_bytes;
  }
  report.run = ++repo_.gc.last_run;
  const EpochManager::GcEpochs epochs = repo_.epochs.BeginGc();
  // Candidates before the snapshot listing, both after BeginGc (6.2 items 1 and 5).
  auto candidates = Candidates(repo_);
  DDS_TRY(candidates.status());
  report.candidates = candidates->size();
  auto listing = repo_.snapshots.List();
  DDS_TRY(listing.status());

  int64_t start = clock.MonotonicNanos();
  DDS_TRY(MarkAll(repo_, report.run, listing->ids, report.mark));  // fail closed
  report.mark_seconds = SecondsSince(clock, start);

  start = clock.MonotonicNanos();
  const auto plan = MakePlan(repo_, *candidates, report.run, epochs.e_min, options.threshold);
  Summarize(plan, report);
  report.plan_seconds = SecondsSince(clock, start);
  DDS_SYNC_POINT("gc.after_plan");
  if (options.dry_run) return report;

  start = clock.MonotonicNanos();
  DDS_TRY(Compact(repo_, plan, report.run, epochs.e_min, report.compact));
  auto reclaimed = repo_.containers.ReclaimRetired(repo_.epochs.OldestReader());
  DDS_TRY(reclaimed.status());
  report.containers_reclaimed += reclaimed->containers;
  report.bytes_reclaimed += reclaimed->file_bytes;
  report.compact_seconds = SecondsSince(clock, start);

  GcTotals& t = repo_.gc.totals;
  ++t.runs;
  t.containers_deleted += report.compact.containers_deleted;
  t.containers_compacted += report.compact.containers_compacted;
  t.blobs_removed += report.compact.blobs_removed;
  t.blobs_copied += report.compact.blobs_copied;
  t.bytes_copied += report.compact.bytes_copied;
  t.containers_reclaimed += report.containers_reclaimed;
  t.bytes_reclaimed += report.bytes_reclaimed;
  return report;
}

}  // namespace dds
