#include "dds/gc/compactor.h"

#include <unordered_map>
#include <utility>

#include "dds/common/check.h"
#include "dds/common/test_hooks.h"
#include "dds/repo/repository.h"
#include "dds/store/container_reader.h"

namespace dds {

namespace {

class Compactor {
 public:
  Compactor(RepoInternals& repo, GcRun run, Epoch e_min, CompactCounters& counters)
      : repo_(repo), run_(run), e_min_(e_min), counters_(counters), writer_(repo.NewWriter()) {}

  // Decides every entry of container `id`, copying the kept ones into the writer.
  Status Process(uint32_t id) {
    auto meta = ReadContainerMetadata(repo_.fs, repo_.containers.PathFor(id), id,
                                      repo_.config.repo_id, repo_.config.chunker.max_size);
    DDS_TRY(meta.status());
    uint64_t kept = 0;
    for (const SealedEntry& entry : meta->entries) {
      switch (repo_.index.RemoveIfDead(entry.fp, entry.loc, run_, e_min_)) {
        case RemoveResult::kRemoved:
          ++counters_.blobs_removed;
          break;
        case RemoveResult::kNotCanonical:
          ++counters_.duplicates_dropped;
          break;
        case RemoveResult::kKept:
          DDS_TRY(Copy(entry));
          ++kept;
          break;
      }
    }
    if (kept == 0) {
      ++counters_.containers_deleted;
    } else {
      ++counters_.containers_compacted;
    }
    if (DDS_MUTATION("gc_unlink_before_copy_sync")) {
      // Broken on purpose (TESTING.md 3.6): unlink before the copies are sealed, so a crash here
      // loses every live blob of this container.
      repo_.containers.Retire(id, repo_.epochs.Advance());
      return repo_.containers.ReclaimRetired(std::nullopt).status();
    }
    done_.push_back(id);
    return Status::Ok();
  }

  // Seals what is left, relocates, then retires every processed container after one Advance:
  // every relocation happens before the retire epoch, so a reader that enters after it finds the
  // new location (6.3).
  Status Finish() {
    DDS_TRY(SealAndRelocate());
    DDS_SYNC_POINT("gc.after_relocate");
    if (done_.empty()) return Status::Ok();
    const Epoch retire_epoch = repo_.epochs.Advance();
    for (uint32_t id : done_) repo_.containers.Retire(id, retire_epoch);
    return Status::Ok();
  }

 private:
  Status Copy(const SealedEntry& entry) {
    // No ReadGuard: only GC unlinks containers, and this one is not retired yet.
    auto bytes = repo_.containers.Read(entry.loc);
    DDS_TRY(bytes.status());
    writer_.Add(entry.fp, entry.loc.type, *bytes);
    moved_.emplace_back(entry.fp, entry.loc);
    ++counters_.blobs_copied;
    counters_.bytes_copied += bytes->size();
    if (writer_.Full()) DDS_TRY(SealAndRelocate());
    return Status::Ok();
  }

  Status SealAndRelocate() {
    if (writer_.Empty()) return Status::Ok();
    auto sealed = writer_.Seal();  // durable before anything points at it (7.2)
    DDS_TRY(sealed.status());
    std::unordered_map<Fingerprint, Location, FingerprintHash> new_location;
    for (const SealedEntry& e : sealed->entries) new_location.emplace(e.fp, e.loc);
    for (const auto& [fp, old_location] : moved_) {
      // Only GC moves or removes entries, and one GC runs at a time, so the entry is still where
      // RemoveIfDead found it (4.3 step 5): a failure is a bug.
      DDS_CHECK(repo_.index.Relocate(fp, old_location, new_location.at(fp)));
    }
    moved_.clear();
    // Published after BeginGc, with an id above every candidate's: not a candidate of this run.
    repo_.containers.MarkPublished(sealed->id, {sealed->payload_bytes, sealed->file_bytes,
                                                static_cast<uint32_t>(sealed->entries.size())});
    return Status::Ok();
  }

  RepoInternals& repo_;
  const GcRun run_;
  const Epoch e_min_;
  CompactCounters& counters_;
  ContainerWriter writer_;
  std::vector<std::pair<Fingerprint, Location>> moved_;  // in the writer, not yet relocated
  std::vector<uint32_t> done_;                           // processed, to retire at the end
};

}  // namespace

Status Compact(RepoInternals& repo, const std::vector<PlannedContainer>& plan, GcRun run,
               Epoch e_min, CompactCounters& counters) {
  Compactor compactor(repo, run, e_min, counters);
  for (const PlannedContainer& p : plan) {
    if (p.action != PlanAction::kKeep) DDS_TRY(compactor.Process(p.candidate.id));
  }
  return compactor.Finish();
}

}  // namespace dds
