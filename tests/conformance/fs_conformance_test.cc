// Fs conformance (TESTING.md 3.2): one suite, run against every Fs implementation. It pins the
// semantics the design relies on. A behavior difference between implementations is a bug in the
// model (MemFs, CrashSimFs), which must behave like PosixFs.

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dds/common/encoding.h"
#include "dds/fs/fs.h"
#include "dds/fs/mem_fs.h"
#include "dds/fs/posix_fs.h"
#include "support/fs_conformance_env.h"

namespace dds {
namespace {

using testing::FsEnv;
using testing::FsEnvFactory;

class FsConformanceTest : public ::testing::TestWithParam<FsEnvFactory> {
 protected:
  void SetUp() override {
    env_ = GetParam().make();
    base_ = env_->base();
  }
  Fs& fs() { return env_->fs(); }
  std::string P(const std::string& name) const { return base_ + "/" + name; }

  void WriteFile(const std::string& path, std::string_view content) {
    auto file = fs().CreateNew(path);
    ASSERT_TRUE(file.ok()) << file.status().ToString();
    ASSERT_TRUE((*file)->Append(AsBytes(content)).ok());
    ASSERT_TRUE((*file)->Sync().ok());
    ASSERT_TRUE((*file)->Close().ok());
  }

  std::string ReadAll(const std::string& path) {
    auto data = ReadWholeFile(fs(), path);
    EXPECT_TRUE(data.ok()) << data.status().ToString();
    if (!data.ok()) return "<error>";
    return std::string(AsChars(*data));
  }

  std::unique_ptr<FsEnv> env_;
  std::string base_;
};

TEST_P(FsConformanceTest, CreateAppendRead) {
  auto file = fs().CreateNew(P("f"));
  ASSERT_TRUE(file.ok()) << file.status().ToString();
  ASSERT_TRUE((*file)->Append(AsBytes("hello")).ok());
  ASSERT_TRUE((*file)->Sync().ok());
  ASSERT_TRUE((*file)->Append(AsBytes("world")).ok());  // appending after a sync continues
  ASSERT_TRUE((*file)->Close().ok());

  auto r = fs().OpenRead(P("f"));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto size = (*r)->Size();
  ASSERT_TRUE(size.ok());
  EXPECT_EQ(*size, 10u);
  std::vector<std::byte> buf(5);
  ASSERT_TRUE((*r)->ReadAt(5, buf).ok());
  EXPECT_EQ(AsChars(buf), "world");
  EXPECT_EQ(ReadAll(P("f")), "helloworld");
}

TEST_P(FsConformanceTest, EmptyFile) {
  WriteFile(P("empty"), "");
  auto r = fs().OpenRead(P("empty"));
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*(*r)->Size(), 0u);
  std::vector<std::byte> none;
  EXPECT_TRUE((*r)->ReadAt(0, none).ok());
}

TEST_P(FsConformanceTest, LargeAppendRoundTrip) {
  std::string big(1 << 20, '\0');
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
  WriteFile(P("big"), big);
  EXPECT_EQ(ReadAll(P("big")), big);
}

TEST_P(FsConformanceTest, CreateNewFailsIfExists) {
  WriteFile(P("f"), "first");
  auto again = fs().CreateNew(P("f"));
  EXPECT_EQ(again.status().code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(ReadAll(P("f")), "first");
  ASSERT_TRUE(fs().CreateDir(P("d")).ok());
  EXPECT_EQ(fs().CreateNew(P("d")).status().code(), StatusCode::kAlreadyExists);
}

TEST_P(FsConformanceTest, CreateNewFailsIfDirectoryMissing) {
  EXPECT_EQ(fs().CreateNew(P("missing/f")).status().code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, ReadPastEndFails) {
  WriteFile(P("f"), "0123456789");
  auto r = fs().OpenRead(P("f"));
  ASSERT_TRUE(r.ok());
  std::vector<std::byte> buf(5);
  EXPECT_TRUE((*r)->ReadAt(5, buf).ok());
  EXPECT_FALSE((*r)->ReadAt(6, buf).ok());
  EXPECT_FALSE((*r)->ReadAt(100, buf).ok());
  std::vector<std::byte> none;
  EXPECT_FALSE((*r)->ReadAt(11, none).ok());
}

TEST_P(FsConformanceTest, OpenReadMissingIsNotFound) {
  EXPECT_EQ(fs().OpenRead(P("nope")).status().code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, RenameReplacesAtomically) {
  WriteFile(P("a"), "A");
  WriteFile(P("b"), "B");
  ASSERT_TRUE(fs().Rename(P("a"), P("b")).ok());
  EXPECT_EQ(ReadAll(P("b")), "A");
  EXPECT_FALSE(*fs().Exists(P("a")));
}

TEST_P(FsConformanceTest, RenameAcrossDirectories) {
  ASSERT_TRUE(fs().CreateDir(P("tmp")).ok());
  ASSERT_TRUE(fs().CreateDir(P("containers")).ok());
  WriteFile(P("tmp/00000001.ctr.tmp"), "payload");
  ASSERT_TRUE(fs().Rename(P("tmp/00000001.ctr.tmp"), P("containers/00000001.ctr")).ok());
  EXPECT_EQ(ReadAll(P("containers/00000001.ctr")), "payload");
  EXPECT_TRUE(fs().ListDir(P("tmp"))->empty());
}

TEST_P(FsConformanceTest, RenameMissingSourceIsNotFound) {
  EXPECT_EQ(fs().Rename(P("nope"), P("x")).code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, LinkCreatesSecondNameForSameFile) {
  WriteFile(P("record"), "holder");
  ASSERT_TRUE(fs().Link(P("record"), P("lock")).ok());
  EXPECT_EQ(ReadAll(P("lock")), "holder");
  ASSERT_TRUE(fs().Remove(P("record")).ok());
  EXPECT_EQ(ReadAll(P("lock")), "holder");
}

TEST_P(FsConformanceTest, LinkFailsIfTargetExistsAndLeavesItUnchanged) {
  WriteFile(P("mine"), "mine");
  WriteFile(P("lock"), "theirs");
  EXPECT_EQ(fs().Link(P("mine"), P("lock")).code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(ReadAll(P("lock")), "theirs");
}

TEST_P(FsConformanceTest, LinkMissingSourceIsNotFound) {
  EXPECT_EQ(fs().Link(P("nope"), P("lock")).code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, RemoveFile) {
  WriteFile(P("f"), "x");
  ASSERT_TRUE(fs().Remove(P("f")).ok());
  EXPECT_FALSE(*fs().Exists(P("f")));
  EXPECT_EQ(fs().Remove(P("f")).code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, RemoveDirectoryFails) {
  ASSERT_TRUE(fs().CreateDir(P("d")).ok());
  EXPECT_FALSE(fs().Remove(P("d")).ok());
  EXPECT_TRUE(*fs().Exists(P("d")));
}

TEST_P(FsConformanceTest, OpenHandleSurvivesRemove) {
  WriteFile(P("f"), "still here");
  auto r = fs().OpenRead(P("f"));
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(fs().Remove(P("f")).ok());
  std::vector<std::byte> buf(10);
  ASSERT_TRUE((*r)->ReadAt(0, buf).ok());
  EXPECT_EQ(AsChars(buf), "still here");
}

TEST_P(FsConformanceTest, OpenHandleSurvivesReplacingRename) {
  WriteFile(P("f"), "old");
  WriteFile(P("g"), "new");
  auto r = fs().OpenRead(P("f"));
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(fs().Rename(P("g"), P("f")).ok());
  std::vector<std::byte> buf(3);
  ASSERT_TRUE((*r)->ReadAt(0, buf).ok());
  EXPECT_EQ(AsChars(buf), "old");
  EXPECT_EQ(ReadAll(P("f")), "new");
}

TEST_P(FsConformanceTest, CreateDirAndListDirReturnsSortedNames) {
  ASSERT_TRUE(fs().CreateDir(P("d")).ok());
  WriteFile(P("d/b"), "");
  WriteFile(P("d/a"), "");
  ASSERT_TRUE(fs().CreateDir(P("d/sub")).ok());
  WriteFile(P("d/sub/deep"), "");  // not a direct child
  auto names = fs().ListDir(P("d"));
  ASSERT_TRUE(names.ok()) << names.status().ToString();
  EXPECT_EQ(*names, (std::vector<std::string>{"a", "b", "sub"}));
}

TEST_P(FsConformanceTest, ListDirEmptyAndMissing) {
  ASSERT_TRUE(fs().CreateDir(P("empty")).ok());
  EXPECT_TRUE(fs().ListDir(P("empty"))->empty());
  EXPECT_EQ(fs().ListDir(P("missing")).status().code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, ListDirOfFileFails) {
  WriteFile(P("f"), "");
  auto names = fs().ListDir(P("f"));
  EXPECT_FALSE(names.ok());
  EXPECT_NE(names.status().code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, CreateDirErrors) {
  ASSERT_TRUE(fs().CreateDir(P("d")).ok());
  EXPECT_EQ(fs().CreateDir(P("d")).code(), StatusCode::kAlreadyExists);
  WriteFile(P("f"), "");
  EXPECT_EQ(fs().CreateDir(P("f")).code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(fs().CreateDir(P("missing/d")).code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, ExistsForFileDirectoryAndMissing) {
  WriteFile(P("f"), "");
  ASSERT_TRUE(fs().CreateDir(P("d")).ok());
  EXPECT_TRUE(*fs().Exists(P("f")));
  EXPECT_TRUE(*fs().Exists(P("d")));
  EXPECT_FALSE(*fs().Exists(P("missing")));
  EXPECT_FALSE(*fs().Exists(P("missing/deeper")));
}

TEST_P(FsConformanceTest, SyncDir) {
  ASSERT_TRUE(fs().CreateDir(P("d")).ok());
  EXPECT_TRUE(fs().SyncDir(P("d")).ok());
  EXPECT_EQ(fs().SyncDir(P("missing")).code(), StatusCode::kNotFound);
}

TEST_P(FsConformanceTest, ConcurrentWritersOfDistinctFiles) {
  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t] {
      const std::string path = P("t" + std::to_string(t));
      auto file = fs().CreateNew(path);
      ASSERT_TRUE(file.ok());
      for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE((*file)->Append(AsBytes(std::to_string(t))).ok());
      }
      ASSERT_TRUE((*file)->Close().ok());
    });
  }
  for (auto& th : threads) th.join();
  for (int t = 0; t < kThreads; ++t) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += std::to_string(t);
    EXPECT_EQ(ReadAll(P("t" + std::to_string(t))), expected);
  }
  EXPECT_EQ(fs().ListDir(base_)->size(), static_cast<size_t>(kThreads));
}

INSTANTIATE_TEST_SUITE_P(AllFs, FsConformanceTest,
                         ::testing::ValuesIn(testing::AllFsEnvFactories()),
                         [](const ::testing::TestParamInfo<FsEnvFactory>& param_info) {
                           return param_info.param.name;
                         });

}  // namespace
}  // namespace dds
