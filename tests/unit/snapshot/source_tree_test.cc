#include "dds/snapshot/source_tree.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "dds/common/encoding.h"
#include "support/temp_dir.h"

namespace dds {
namespace {

std::string ReadAll(ByteSource& source) {
  std::string out;
  std::vector<std::byte> buf(7);
  while (true) {
    auto n = source.Read(buf);
    EXPECT_TRUE(n.ok());
    if (!n.ok() || *n == 0) break;
    out += AsChars(std::span(buf).first(*n));
  }
  return out;
}

TEST(PosixSourceTreeTest, ListsKindsModesTimesAndSizesSortedByName) {
  testing::TempDir dir;
  std::filesystem::create_directory(dir.Sub("b-dir"));
  std::ofstream(dir.Sub("a-file")) << "hello";
  ::chmod(dir.Sub("a-file").c_str(), 0640);
  std::filesystem::create_symlink("../somewhere", dir.Sub("c-link"));
  ASSERT_EQ(::mkfifo(dir.Sub("d-fifo").c_str(), 0600), 0);
  const struct timespec times[2] = {{1000, 0}, {1700000000, 123456789}};
  ASSERT_EQ(::utimensat(AT_FDCWD, dir.Sub("a-file").c_str(), times, 0), 0);

  PosixSourceTree tree(dir.path());
  auto entries = tree.ListDir("");
  ASSERT_TRUE(entries.ok()) << entries.status().ToString();
  ASSERT_EQ(entries->size(), 4u);
  EXPECT_EQ((*entries)[0].name, "a-file");
  EXPECT_EQ((*entries)[0].kind, SourceKind::kFile);
  EXPECT_EQ((*entries)[0].mode, 0640u);
  EXPECT_EQ((*entries)[0].size, 5u);
  EXPECT_EQ((*entries)[0].mtime_sec, 1700000000);
  EXPECT_EQ((*entries)[0].mtime_nsec, 123456789u);
  EXPECT_EQ((*entries)[1].kind, SourceKind::kDirectory);
  EXPECT_EQ((*entries)[2].kind, SourceKind::kSymlink);
  EXPECT_EQ((*entries)[3].kind, SourceKind::kOther);  // a FIFO is skipped, not backed up

  auto link = tree.ReadLink("c-link");
  ASSERT_TRUE(link.ok());
  EXPECT_EQ(*link, "../somewhere");
  auto file = tree.OpenFile("a-file");
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(ReadAll(**file), "hello");
  EXPECT_TRUE(tree.ListDir("b-dir")->empty());
}

TEST(PosixSourceTreeTest, OpenFileNeverFollowsASymlink) {
  testing::TempDir dir;
  std::ofstream(dir.Sub("real")) << "secret";
  std::filesystem::create_symlink("real", dir.Sub("link"));
  PosixSourceTree tree(dir.path());
  EXPECT_FALSE(tree.OpenFile("link").ok());
}

TEST(PosixSourceTreeTest, MissingPathsAreNotFound) {
  testing::TempDir dir;
  PosixSourceTree tree(dir.path());
  EXPECT_EQ(tree.ListDir("nope").status().code(), StatusCode::kNotFound);
  EXPECT_EQ(tree.OpenFile("nope").status().code(), StatusCode::kNotFound);
}

TEST(MemSourceTreeTest, BehavesLikeADirectoryTree) {
  MemSourceTree tree;
  tree.AddDir("d", 0700, 5, 6);
  tree.AddFile("d/f", {std::byte{'x'}, std::byte{'y'}}, 0600, 7, 8);
  tree.AddSymlink("l", "d/f");
  tree.AddSpecial("p");
  tree.AddFile("\xff-bytes", {});
  auto root = tree.ListDir("");
  ASSERT_TRUE(root.ok());
  ASSERT_EQ(root->size(), 4u);
  EXPECT_EQ((*root)[0].name, "d");
  EXPECT_EQ((*root)[1].name, "l");
  EXPECT_EQ((*root)[2].kind, SourceKind::kOther);
  EXPECT_EQ((*root)[3].name, "\xff-bytes");  // sorted as unsigned bytes: 0xff after 'p'
  auto sub = tree.ListDir("d");
  ASSERT_TRUE(sub.ok());
  ASSERT_EQ(sub->size(), 1u);
  EXPECT_EQ((*sub)[0].size, 2u);
  EXPECT_EQ((*sub)[0].mtime_nsec, 8u);
  auto f = tree.OpenFile("d/f");
  ASSERT_TRUE(f.ok());
  EXPECT_EQ(ReadAll(**f), "xy");
  EXPECT_EQ(*tree.ReadLink("l"), "d/f");
  EXPECT_EQ(tree.ListDir("missing").status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace dds
