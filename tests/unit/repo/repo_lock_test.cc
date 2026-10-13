// The repository lock (D16): Busy naming a live holder, takeover of a dead holder on
// the same host, never across hosts.

#include "dds/repo/repo_lock.h"

#include <gtest/gtest.h>

#include <string>

#include "dds/common/clock.h"
#include "dds/common/encoding.h"
#include "dds/fs/mem_fs.h"
#include "support/fake_process_identity.h"

namespace dds {
namespace {

class RepoLockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fs_.CreateDir("/r").ok());
    ASSERT_TRUE(fs_.CreateDir("/r/tmp").ok());
  }
  Result<std::unique_ptr<RepoLock>> Acquire(const ProcessIdentity& who) {
    return RepoLock::Acquire(fs_, layout_, who, clock_, "dds test");
  }
  void WriteLockFile(const std::string& text) {
    auto f = fs_.CreateNew("/r/lock");
    ASSERT_TRUE(f.ok());
    ASSERT_TRUE((*f)->Append(AsBytes(text)).ok());
    ASSERT_TRUE((*f)->Close().ok());
  }
  std::string LockFile() {
    auto data = ReadWholeFile(fs_, "/r/lock");
    return data.ok() ? std::string(AsChars(*data)) : "<none>";
  }

  MemFs fs_;
  RepoLayout layout_{"/r"};
  ManualClock clock_{1000, 1};
};

TEST_F(RepoLockTest, AcquireWritesTheRecordAndReleaseRemovesIt) {
  testing::FakeProcessIdentity me("host-a", 100, 5000);
  auto lock = Acquire(me);
  ASSERT_TRUE(lock.ok()) << lock.status().ToString();
  auto record = DecodeLockRecord(LockFile());
  ASSERT_TRUE(record.ok());
  EXPECT_EQ(record->host, "host-a");
  EXPECT_EQ(record->pid, 100u);
  EXPECT_EQ(record->pid_start, 5000u);
  EXPECT_EQ(record->command, "dds test");
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());  // the temporary record is gone; `lock` remains
  ASSERT_TRUE((*lock)->Release().ok());
  EXPECT_FALSE(*fs_.Exists("/r/lock"));
}

TEST_F(RepoLockTest, SecondAcquireIsBusyAndNamesTheHolder) {
  testing::FakeProcessIdentity first("host-a", 100, 5000);
  testing::FakeProcessIdentity second("host-a", 200, 6000);
  second.SetAlive(100, 5000, true);
  auto lock = Acquire(first);
  ASSERT_TRUE(lock.ok());
  auto again = Acquire(second);
  ASSERT_EQ(again.status().code(), StatusCode::kBusy);
  EXPECT_NE(again.status().message().find("pid 100"), std::string::npos)
      << again.status().message();
  EXPECT_NE(again.status().message().find("dds test"), std::string::npos);
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());  // the loser cleaned up its record
}

TEST_F(RepoLockTest, DeadHolderOnTheSameHostIsTakenOver) {
  // A crashed process leaves its record behind (writing it directly, rather than leaking a
  // RepoLock, keeps LeakSanitizer quiet).
  WriteLockFile(EncodeLockRecord({"host-a", 100, 5000, 1, "dds crashed"}));
  testing::FakeProcessIdentity next("host-a", 300, 7000);  // pid 100 is not alive for `next`
  auto lock = Acquire(next);
  ASSERT_TRUE(lock.ok()) << lock.status().ToString();
  EXPECT_EQ(DecodeLockRecord(LockFile())->pid, 300u);
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());
}

TEST_F(RepoLockTest, ReusedPidIsTreatedAsDead) {
  WriteLockFile(EncodeLockRecord({"host-a", 100, 5000, 1, "dds old"}));
  testing::FakeProcessIdentity next("host-a", 300, 7000);
  next.SetAlive(100, 9999, true);  // pid 100 is alive, but it started at 9999: another process
  auto lock = Acquire(next);
  ASSERT_TRUE(lock.ok()) << lock.status().ToString();
  EXPECT_EQ(DecodeLockRecord(LockFile())->pid, 300u);
}

TEST_F(RepoLockTest, LockFromAnotherHostIsNeverTakenOver) {
  WriteLockFile(EncodeLockRecord({"host-b", 100, 5000, 1, "dds serve"}));
  testing::FakeProcessIdentity me("host-a", 300, 7000);  // host-b's pid 100 is not alive here
  auto lock = Acquire(me);
  ASSERT_EQ(lock.status().code(), StatusCode::kBusy);
  EXPECT_NE(lock.status().message().find("host-b"), std::string::npos);
  EXPECT_EQ(DecodeLockRecord(LockFile())->host, "host-b");  // untouched
}

TEST_F(RepoLockTest, UnparsableLockIsNeverTakenOver) {
  WriteLockFile("garbage");
  testing::FakeProcessIdentity me("host-a", 300, 7000);
  EXPECT_EQ(Acquire(me).status().code(), StatusCode::kBusy);
  EXPECT_EQ(LockFile(), "garbage");
}

TEST_F(RepoLockTest, LosingATakeoverRaceEndsInBusyAndRestoresTheWinnersLock) {
  // `me` judges pid 100's lock stale, but just then another process takes it over first and
  // installs its own record. `me` must notice that the lock it moved away is not the stale one,
  // put the winner's lock back, and report Busy (D16).
  WriteLockFile(EncodeLockRecord({"host-a", 100, 5000, 1, "dds crashed"}));
  const std::string winner = EncodeLockRecord({"host-a", 400, 8000, 2, "dds winner"});
  testing::FakeProcessIdentity me("host-a", 300, 7000);
  bool raced = false;
  me.on_is_alive = [&] {
    if (raced) return;
    raced = true;
    ASSERT_TRUE(fs_.Remove("/r/lock").ok());
    WriteLockFile(winner);
  };
  auto lock = Acquire(me);
  ASSERT_EQ(lock.status().code(), StatusCode::kBusy);
  EXPECT_EQ(LockFile(), winner);
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());
}

}  // namespace
}  // namespace dds
