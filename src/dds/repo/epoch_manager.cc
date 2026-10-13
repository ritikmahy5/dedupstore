#include "dds/repo/epoch_manager.h"

#include <algorithm>

#include "dds/common/check.h"

namespace dds {

Epoch EpochManager::Advance() {
  MutexLock lock(mu_);
  // The load can be relaxed: only this mutex's holder writes the counter. The store is release so
  // that a reader's acquire load of the new value sees what this thread did before advancing.
  const Epoch next = counter_.load(std::memory_order_relaxed) + 1;
  counter_.store(next, std::memory_order_release);
  return next;
}

Epoch EpochManager::BeginSession() {
  MutexLock lock(mu_);
  const Epoch start = counter_.load(std::memory_order_relaxed);  // mu_ orders it, as above
  sessions_.insert(start);
  return start;
}

void EpochManager::EndSession(Epoch start) {
  MutexLock lock(mu_);
  auto it = sessions_.find(start);
  DDS_CHECK(it != sessions_.end());
  sessions_.erase(it);
}

Epoch EpochManager::EnterRead() {
  MutexLock lock(mu_);
  const Epoch entry = counter_.load(std::memory_order_relaxed);  // mu_ orders it, as above
  readers_.insert(entry);
  return entry;
}

void EpochManager::ExitRead(Epoch entry) {
  MutexLock lock(mu_);
  auto it = readers_.find(entry);
  DDS_CHECK(it != readers_.end());
  readers_.erase(it);
}

EpochManager::GcEpochs EpochManager::BeginGc() {
  MutexLock lock(mu_);
  const Epoch e_gc = counter_.load(std::memory_order_relaxed) + 1;
  counter_.store(e_gc, std::memory_order_release);
  // A multiset is ordered, so its first element is the earliest active session's start.
  const Epoch e_min = sessions_.empty() ? e_gc : std::min(e_gc, *sessions_.begin());
  return {e_gc, e_min};
}

std::optional<Epoch> EpochManager::OldestReader() const {
  MutexLock lock(mu_);
  if (readers_.empty()) return std::nullopt;
  return *readers_.begin();
}

size_t EpochManager::active_sessions() const {
  MutexLock lock(mu_);
  return sessions_.size();
}

}  // namespace dds
