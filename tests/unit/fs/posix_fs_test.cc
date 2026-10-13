#include "dds/fs/posix_fs.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "dds/common/unique_fd.h"
#include "support/temp_dir.h"

namespace dds {
namespace {

TEST(PosixFsTest, ErrnoMapping) {
  EXPECT_EQ(ErrnoStatus(ENOENT, "open", "/x").code(), StatusCode::kNotFound);
  EXPECT_EQ(ErrnoStatus(EEXIST, "link", "/x").code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(ErrnoStatus(EIO, "fsync", "/x").code(), StatusCode::kIoError);
  EXPECT_EQ(ErrnoStatus(EACCES, "open", "/x").code(), StatusCode::kIoError);
}

TEST(PosixFsTest, ErrorsNameTheOperationAndPath) {
  testing::TempDir dir;
  PosixFs fs;
  const std::string missing = dir.Sub("missing");
  Status s = fs.OpenRead(missing).status();
  EXPECT_EQ(s.code(), StatusCode::kNotFound);
  EXPECT_NE(s.message().find(missing), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("open"), std::string::npos) << s.message();
}

#if defined(__APPLE__)
// D23: SyncDir uses fcntl(F_FULLFSYNC) on macOS only because it succeeds on a directory
// descriptor. This test re-checks that on every run, so a macOS change cannot silently weaken
// directory durability.
TEST(PosixFsTest, FullFsyncSucceedsOnDirectoryDescriptor) {
  testing::TempDir dir;
  UniqueFd fd(::open(dir.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  ASSERT_TRUE(fd.valid()) << std::strerror(errno);
  EXPECT_EQ(::fcntl(fd.get(), F_FULLFSYNC), 0) << std::strerror(errno);
}
#endif

}  // namespace
}  // namespace dds
