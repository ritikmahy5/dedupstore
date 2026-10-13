#include "dds/common/ordered_mutex.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <thread>

namespace dds {
namespace {

TEST(OrderedMutexTest, AscendingLevelsAreAllowed) {
  OrderedMutex gc(lock_level::kGc, "gc");
  OrderedMutex epoch(lock_level::kEpoch, "epoch");
  OrderedMutex shard(lock_level::kShard, "shard");
  MutexLock a(gc);
  MutexLock b(epoch);
  MutexLock c(shard);
  SUCCEED();
}

TEST(OrderedMutexTest, ReleaseThenAcquireLowerIsAllowed) {
  OrderedMutex low(lock_level::kSnapshots, "snapshots");
  OrderedMutex high(lock_level::kContainers, "containers");
  {
    MutexLock h(high);
  }
  MutexLock l(low);
  MutexLock h(high);
  SUCCEED();
}

TEST(OrderedMutexTest, ConditionVariableWaitReleasesAndReacquires) {
  OrderedMutex mu(lock_level::kLeaf, "queue");
  std::condition_variable_any cv;
  bool ready = false;
  std::thread producer([&] {
    MutexLock lock(mu);
    ready = true;
    cv.notify_all();
  });
  {
    MutexLock lock(mu);
    while (!ready) cv.wait(lock);
  }
  producer.join();
  // After the wait the lock was released cleanly: a lower level can be taken now.
  OrderedMutex low(lock_level::kGc, "gc");
  MutexLock l(low);
  EXPECT_TRUE(ready);
}

TEST(OrderedMutexTest, CheckerStateIsPerThread) {
  OrderedMutex high(lock_level::kShard, "shard");
  OrderedMutex low(lock_level::kGc, "gc");
  MutexLock h(high);
  // Another thread holds nothing, so it may take a low level while this one holds a high one.
  std::thread t([&] { MutexLock l(low); });
  t.join();
  SUCCEED();
}

TEST(OrderedMutexDeathTest, DescendingAcquisitionAborts) {
  if (!LockOrderCheckEnabled()) GTEST_SKIP() << "lock-order checker is off in this preset";
  EXPECT_DEATH(
      {
        OrderedMutex session(lock_level::kSession, "session");
        OrderedMutex epoch(lock_level::kEpoch, "epoch");
        MutexLock a(session);
        MutexLock b(epoch);  // 30 after 40: out of order
      },
      "lock order violation.*epoch.*session");
}

TEST(OrderedMutexDeathTest, EqualLevelsAbort) {
  // Two shard mutexes must never be held at once (ARCHITECTURE.md 5.3).
  if (!LockOrderCheckEnabled()) GTEST_SKIP() << "lock-order checker is off in this preset";
  EXPECT_DEATH(
      {
        OrderedMutex a(lock_level::kShard, "shard-a");
        OrderedMutex b(lock_level::kShard, "shard-b");
        MutexLock la(a);
        MutexLock lb(b);
      },
      "lock order violation");
}

}  // namespace
}  // namespace dds
