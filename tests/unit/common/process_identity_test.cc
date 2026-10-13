#include "dds/common/process_identity.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dds {
namespace {

TEST(ProcessIdentityTest, SelfIsAlive) {
  PosixProcessIdentity self;
  EXPECT_FALSE(self.Host().empty());
  EXPECT_EQ(self.Pid(), static_cast<uint64_t>(getpid()));
  EXPECT_GT(self.PidStart(), 0u);
  EXPECT_EQ(ProcessStartTime(self.Pid()), self.PidStart());
  EXPECT_TRUE(self.IsAlive(self.Pid(), self.PidStart()));
}

TEST(ProcessIdentityTest, SameMachineWrongStartTimeIsAReusedPid) {
  PosixProcessIdentity self;
  // Our pid with another start time: a different (earlier) process that had this pid.
  EXPECT_FALSE(self.IsAlive(self.Pid(), self.PidStart() + 1));
}

TEST(ProcessIdentityTest, ExitedProcessIsDead) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) _exit(0);
  const auto start = ProcessStartTime(static_cast<uint64_t>(child));
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);  // reaped: the pid no longer exists
  PosixProcessIdentity self;
  EXPECT_FALSE(self.IsAlive(static_cast<uint64_t>(child), start.value_or(1)));
  EXPECT_EQ(ProcessStartTime(static_cast<uint64_t>(child)), std::nullopt);
}

}  // namespace
}  // namespace dds
