#pragma once

#include <mutex>

#include "dds/common/thread_annotations.h"

namespace dds {

// Lock levels (ARCHITECTURE.md 5.3). A thread may acquire a mutex only if its level is higher
// than every level the thread already holds, which rules out lock-order deadlocks by
// construction. Equal levels are therefore never held together (two shards, two sessions).
namespace lock_level {
inline constexpr int kGc = 10;          // gc_mutex: one GC at a time
inline constexpr int kSnapshots = 20;   // snapshots_mutex: manifest listing, commit, removal
inline constexpr int kEpoch = 30;       // epoch_mutex: epoch counter and registrations
inline constexpr int kSession = 40;     // a shard mutex of one IngestSession's pending map
inline constexpr int kContainers = 50;  // containers_mutex: ContainerSet state
inline constexpr int kShard = 60;       // shard_mutex[i] of ChunkIndex
inline constexpr int kFsModel = 90;     // CrashSimFs: held while it calls into its MemFs (D77)
inline constexpr int kLeaf = 100;       // queues, sender window, link pipes, Fs internals
}  // namespace lock_level

// True when this build checks lock levels (debug, asan, tsan; not release). Tests that need the
// checker GTEST_SKIP() without it.
bool LockOrderCheckEnabled();

// A std::mutex with a lock level. When the checker is compiled in (DDS_ENABLE_LOCK_ORDER_CHECK,
// D59), lock() aborts through internal::Fatal before blocking if the calling thread already holds
// a mutex of the same or a higher level, naming both mutexes. Every mutex in dds is an
// OrderedMutex. Take it with MutexLock.
class DDS_CAPABILITY("mutex") OrderedMutex {
 public:
  // `name` must outlive the mutex (use a string literal). It appears in violation reports.
  OrderedMutex(int level, const char* name) : level_(level), name_(name) {}
  OrderedMutex(const OrderedMutex&) = delete;
  OrderedMutex& operator=(const OrderedMutex&) = delete;

  void lock() DDS_ACQUIRE();
  void unlock() DDS_RELEASE();

  int level() const { return level_; }
  const char* name() const { return name_; }

 private:
  std::mutex mu_;
  const int level_;
  const char* const name_;
};

// RAII lock on an OrderedMutex, visible to clang's thread-safety analysis. (libstdc++'s
// std::scoped_lock and std::unique_lock carry no annotations, so the analysis cannot see them;
// D56.) It also meets the Lockable requirements, so std::condition_variable_any can wait on it:
//
//   MutexLock lock(mu_);
//   while (!ready_) cv_.wait(lock);   // a loop, not a predicate lambda: the analysis does not
//                                     // know a lambda runs with the lock held
class DDS_SCOPED_CAPABILITY MutexLock {
 public:
  explicit MutexLock(OrderedMutex& mu) DDS_ACQUIRE(mu) : mu_(mu) { mu_.lock(); }
  ~MutexLock() DDS_RELEASE() {
    if (locked_) mu_.unlock();
  }
  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

  // Used by condition_variable_any::wait, and by code that must drop the lock around I/O.
  void lock() DDS_ACQUIRE() {
    mu_.lock();
    locked_ = true;
  }
  void unlock() DDS_RELEASE() {
    mu_.unlock();
    locked_ = false;
  }

 private:
  OrderedMutex& mu_;
  bool locked_ = true;
};

}  // namespace dds
