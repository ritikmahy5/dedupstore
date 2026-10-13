#pragma once

#include <atomic>
#include <optional>
#include <set>

#include "dds/common/epoch.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"

namespace dds {

// The process-wide epoch counter and the registrations that make concurrent GC safe
// (ARCHITECTURE.md 6.1, exact definitions): ingest sessions, readers, and GC's start. The counter
// is 1 on open, only increases, and is modified only under epoch_mutex; each registration reads it
// and registers in one critical section (D50). Thread-safe.
class EpochManager {
 public:
  // Acquire load: pairs with the release store in Advance, so a thread that sees a new value also
  // sees everything that happened before it was published.
  Epoch Current() const { return counter_.load(std::memory_order_acquire); }

  // counter = counter + 1. Returns the new value (D50).
  Epoch Advance() DDS_EXCLUDES(mu_);

  // A session registers before its first lookup or publish and deregisters after its commit or
  // abort is complete. Returns the session's start epoch.
  Epoch BeginSession() DDS_EXCLUDES(mu_);
  void EndSession(Epoch start) DDS_EXCLUDES(mu_);

  // A reader registers before its index lookup and leaves after its container read (6.3).
  // Returns the reader's entry epoch.
  Epoch EnterRead() DDS_EXCLUDES(mu_);
  void ExitRead(Epoch entry) DDS_EXCLUDES(mu_);

  struct GcEpochs {
    Epoch e_gc = 0;
    Epoch e_min = 0;  // min(e_gc, start of every active session)
  };
  GcEpochs BeginGc() DDS_EXCLUDES(mu_);

  // The smallest entry epoch of any active reader, or nullopt when nobody is reading. A container
  // retired at epoch R may be unlinked once this is nullopt or >= R (6.3).
  std::optional<Epoch> OldestReader() const DDS_EXCLUDES(mu_);
  size_t active_sessions() const DDS_EXCLUDES(mu_);

 private:
  mutable OrderedMutex mu_{lock_level::kEpoch, "epoch"};
  // Written only under mu_ (so increments never race), read with acquire anywhere.
  std::atomic<Epoch> counter_{1};
  std::multiset<Epoch> sessions_ DDS_GUARDED_BY(mu_);  // start epochs of active sessions
  std::multiset<Epoch> readers_ DDS_GUARDED_BY(mu_);   // entry epochs of active readers
};

// Registers a reader for one lookup-then-read unit: one tree blob, one file, or one replication
// batch, never a whole snapshot, so a slow reader delays reclamation by one unit at most (D21).
class ReadGuard {
 public:
  explicit ReadGuard(EpochManager& epochs) : epochs_(epochs), entry_(epochs.EnterRead()) {}
  ~ReadGuard() { epochs_.ExitRead(entry_); }
  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;

 private:
  EpochManager& epochs_;
  const Epoch entry_;
};

}  // namespace dds
