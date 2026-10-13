#include "dds/common/unique_fd.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace dds {
namespace {

bool IsOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

int OpenDevNull() { return open("/dev/null", O_RDONLY | O_CLOEXEC); }

TEST(UniqueFdTest, ClosesOnDestruction) {
  int raw = OpenDevNull();
  ASSERT_GE(raw, 0);
  {
    UniqueFd fd(raw);
    EXPECT_TRUE(fd.valid());
    EXPECT_EQ(fd.get(), raw);
    EXPECT_TRUE(IsOpen(raw));
  }
  EXPECT_FALSE(IsOpen(raw));
}

TEST(UniqueFdTest, MoveTransfersOwnership) {
  int raw = OpenDevNull();
  ASSERT_GE(raw, 0);
  UniqueFd a(raw);
  UniqueFd b(std::move(a));
  EXPECT_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): testing the moved-from state
  EXPECT_EQ(b.get(), raw);
  UniqueFd c;
  c = std::move(b);
  EXPECT_EQ(c.get(), raw);
  EXPECT_TRUE(IsOpen(raw));
}

TEST(UniqueFdTest, ReleaseGivesUpOwnership) {
  int raw = OpenDevNull();
  ASSERT_GE(raw, 0);
  {
    UniqueFd fd(raw);
    EXPECT_EQ(fd.Release(), raw);
    EXPECT_FALSE(fd.valid());
  }
  EXPECT_TRUE(IsOpen(raw));
  close(raw);
}

TEST(UniqueFdTest, CloseReportsSuccessAndInvalidates) {
  UniqueFd fd(OpenDevNull());
  ASSERT_TRUE(fd.valid());
  EXPECT_TRUE(fd.Close().ok());
  EXPECT_FALSE(fd.valid());
  EXPECT_TRUE(fd.Close().ok());  // closing an empty UniqueFd is a no-op
}

}  // namespace
}  // namespace dds
