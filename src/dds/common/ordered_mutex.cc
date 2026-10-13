#include "dds/common/ordered_mutex.h"

#include <array>

#include "dds/common/check.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

#ifdef DDS_ENABLE_LOCK_ORDER_CHECK
constexpr bool kCheckEnabled = true;
#else
constexpr bool kCheckEnabled = false;
#endif

// The mutexes the current thread holds, in acquisition order. The deepest legal nesting is the
// seven levels of ARCHITECTURE.md 5.3, so a small fixed array is enough and never allocates.
struct HeldLocks {
  std::array<const OrderedMutex*, 16> mutexes{};
  size_t count = 0;
};

thread_local HeldLocks t_held;

void CheckCanAcquire(const OrderedMutex& next) {
  for (size_t i = 0; i < t_held.count; ++i) {
    const OrderedMutex* held = t_held.mutexes[i];
    if (held->level() >= next.level()) {
      internal::Fatal(
          __FILE__, __LINE__,
          StrCat("lock order violation: acquiring '", next.name(), "' (level ", next.level(),
                 ") while holding '", held->name(), "' (level ", held->level(), ")"));
    }
  }
  DDS_CHECK(t_held.count < t_held.mutexes.size());
}

void RecordRelease(const OrderedMutex& mu) {
  // Releases need not be in reverse order (a MutexLock may unlock early), so search.
  for (size_t i = t_held.count; i > 0; --i) {
    if (t_held.mutexes[i - 1] == &mu) {
      for (size_t j = i - 1; j + 1 < t_held.count; ++j) t_held.mutexes[j] = t_held.mutexes[j + 1];
      --t_held.count;
      return;
    }
  }
  internal::Fatal(__FILE__, __LINE__, StrCat("releasing '", mu.name(), "', which is not held"));
}

}  // namespace

bool LockOrderCheckEnabled() { return kCheckEnabled; }

void OrderedMutex::lock() {
  // Check before blocking, so a wrong order aborts with a report instead of deadlocking.
  if (kCheckEnabled) CheckCanAcquire(*this);
  mu_.lock();
  if (kCheckEnabled) t_held.mutexes[t_held.count++] = this;
}

void OrderedMutex::unlock() {
  if (kCheckEnabled) RecordRelease(*this);
  mu_.unlock();
}

}  // namespace dds
