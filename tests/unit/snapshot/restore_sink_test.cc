#include "dds/snapshot/restore_sink.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "dds/common/encoding.h"
#include "support/temp_dir.h"

namespace dds {
namespace {

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

struct stat Lstat(const std::string& path) {
  struct stat st{};
  EXPECT_EQ(::lstat(path.c_str(), &st), 0) << path;
  return st;
}

int64_t MtimeSec(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec.tv_sec;
#else
  return st.st_mtim.tv_sec;
#endif
}

long MtimeNsec(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec.tv_nsec;
#else
  return st.st_mtim.tv_nsec;
#endif
}

TEST(PosixRestoreSinkTest, CreatesEntriesWithModesAndTimes) {
  testing::TempDir dir;
  const std::string dest = dir.Sub("out");
  PosixRestoreSink sink(dest);
  auto root = sink.OpenRoot();
  ASSERT_TRUE(root.ok()) << root.status().ToString();
  auto sub = sink.MakeDir(*root, "ro");
  ASSERT_TRUE(sub.ok());
  auto file = sink.CreateFile(*sub, "f");
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE((*file)->Write(AsBytes("data")).ok());
  ASSERT_TRUE((*file)->Finish({0444, 1700000000, 5}).ok());
  ASSERT_TRUE(sink.MakeSymlink(*root, "l", "ro/f", {0777, 1600000000, 7}).ok());
  // The directory is read-only only after its children exist.
  ASSERT_TRUE(sink.FinishDir(*sub, EntryMeta{0555, 1500000000, 9}).ok());
  ASSERT_TRUE(sink.FinishDir(*root, std::nullopt).ok());

  EXPECT_EQ(Slurp(dest + "/ro/f"), "data");
  EXPECT_EQ(Lstat(dest + "/ro/f").st_mode & 07777, 0444u);
  EXPECT_EQ(MtimeSec(Lstat(dest + "/ro/f")), 1700000000);
  EXPECT_EQ(MtimeNsec(Lstat(dest + "/ro/f")), 5);
  EXPECT_EQ(Lstat(dest + "/ro").st_mode & 07777, 0555u);
  EXPECT_EQ(MtimeSec(Lstat(dest + "/ro")), 1500000000);
  EXPECT_TRUE(S_ISLNK(Lstat(dest + "/l").st_mode));
  EXPECT_EQ(std::filesystem::read_symlink(dest + "/l"), "ro/f");
  EXPECT_EQ(MtimeSec(Lstat(dest + "/l")), 1600000000);  // the link's own time, not its target's
}

TEST(PosixRestoreSinkTest, NeverReplacesAnything) {
  testing::TempDir dir;
  PosixRestoreSink sink(dir.Sub("out"));
  auto root = sink.OpenRoot();
  ASSERT_TRUE(root.ok());
  ASSERT_TRUE(sink.MakeDir(*root, "x").ok());
  EXPECT_EQ(sink.MakeDir(*root, "x").status().code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(sink.CreateFile(*root, "x").status().code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(sink.MakeSymlink(*root, "x", "t", {}).code(), StatusCode::kAlreadyExists);
}

TEST(PosixRestoreSinkTest, NeverFollowsASymlinkItCreated) {
  // A symlink "esc" pointing outside the destination, then a file and a directory of the same
  // name: both must fail instead of writing through the link (G10, D42).
  testing::TempDir dir;
  std::filesystem::create_directory(dir.Sub("outside"));
  PosixRestoreSink sink(dir.Sub("out"));
  auto root = sink.OpenRoot();
  ASSERT_TRUE(root.ok());
  ASSERT_TRUE(sink.MakeSymlink(*root, "esc", dir.Sub("outside"), {}).ok());
  EXPECT_FALSE(sink.CreateFile(*root, "esc").ok());
  EXPECT_FALSE(sink.MakeDir(*root, "esc").ok());
  // A dangling link: following it with O_CREAT would create a file outside the destination.
  ASSERT_TRUE(sink.MakeSymlink(*root, "dangling", dir.Sub("outside/planted"), {}).ok());
  EXPECT_FALSE(sink.CreateFile(*root, "dangling").ok());
  EXPECT_TRUE(std::filesystem::is_empty(dir.Sub("outside")));
}

TEST(PosixRestoreSinkTest, RejectsInvalidNames) {
  testing::TempDir dir;
  PosixRestoreSink sink(dir.Sub("out"));
  auto root = sink.OpenRoot();
  ASSERT_TRUE(root.ok());
  for (const char* bad : {"..", ".", "a/b", ""}) {
    EXPECT_EQ(sink.MakeDir(*root, bad).status().code(), StatusCode::kInvalidArgument) << bad;
    EXPECT_EQ(sink.CreateFile(*root, bad).status().code(), StatusCode::kInvalidArgument) << bad;
  }
}

TEST(PosixRestoreSinkTest, DestinationMustBeAbsentOrEmpty) {
  testing::TempDir dir;
  std::filesystem::create_directory(dir.Sub("empty"));
  EXPECT_TRUE(PosixRestoreSink(dir.Sub("empty")).OpenRoot().ok());
  std::filesystem::create_directory(dir.Sub("full"));
  std::ofstream(dir.Sub("full/x")) << "x";
  EXPECT_EQ(PosixRestoreSink(dir.Sub("full")).OpenRoot().status().code(),
            StatusCode::kAlreadyExists);
  std::ofstream(dir.Sub("file")) << "x";
  EXPECT_EQ(PosixRestoreSink(dir.Sub("file")).OpenRoot().status().code(),
            StatusCode::kAlreadyExists);
}

TEST(MemRestoreSinkTest, RecordsTheTreeAndNeverReplaces) {
  MemRestoreSink sink;
  auto root = sink.OpenRoot();
  ASSERT_TRUE(root.ok());
  auto d = sink.MakeDir(*root, "d");
  ASSERT_TRUE(d.ok());
  auto f = sink.CreateFile(*d, "f");
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE((*f)->Write(AsBytes("ab")).ok());
  ASSERT_TRUE((*f)->Write(AsBytes("c")).ok());
  ASSERT_TRUE((*f)->Finish({0600, 3, 4}).ok());
  ASSERT_TRUE(sink.MakeSymlink(*root, "l", "d/f", {0777, 5, 6}).ok());
  ASSERT_TRUE(sink.FinishDir(*d, EntryMeta{0700, 1, 2}).ok());
  EXPECT_EQ(sink.MakeDir(*root, "d").status().code(), StatusCode::kAlreadyExists);
  ASSERT_TRUE(sink.FinishDir(*root, std::nullopt).ok());
  const MemTree& t = sink.nodes();
  ASSERT_EQ(t.size(), 3u);
  EXPECT_EQ(t.at("d").kind, EntryKind::kDirectory);
  EXPECT_EQ(t.at("d").mode, 0700u);
  EXPECT_EQ(t.at("d/f").content.size(), 3u);
  EXPECT_EQ(t.at("d/f").mtime_nsec, 4u);
  EXPECT_EQ(t.at("l").target, "d/f");
}

}  // namespace
}  // namespace dds
