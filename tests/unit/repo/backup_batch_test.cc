// The walker queues files in batches (D102). Every file must still be stored exactly once whatever
// the batch boundaries: the batch that fills up, the partial batch at the end of the walk, and a
// batch closed early by its byte limit.

#include <gtest/gtest.h>

#include <string>

#include "dds/repo/backup.h"
#include "dds/snapshot/source_tree.h"
#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

class BackupBatchTest : public testing::RepoTest {
 protected:
  // Backs up `source` with `threads` workers and checks the restored tree and the file count.
  void RoundTrip(MemSourceTree& source, size_t threads, uint64_t files) {
    BackupOptions options;
    options.threads = threads;
    auto b = repo_->Backup(source, options);
    ASSERT_TRUE(b.ok()) << b.status().ToString();
    EXPECT_EQ(b->manifest.file_count, files);
    const auto diffs = testing::DiffMemTrees(source.nodes(), RestoreOk(b->id));
    EXPECT_TRUE(diffs.empty()) << ::testing::PrintToString(diffs);
  }
};

TEST_F(BackupBatchTest, FileCountsAroundTheBatchSizeAreAllStored) {
  const uint64_t n = kFileBatchFiles;
  for (uint64_t files : {uint64_t{0}, uint64_t{1}, n - 1, n, n + 1, 2 * n + 1}) {
    for (size_t threads : {size_t{1}, size_t{4}}) {
      SCOPED_TRACE(std::to_string(files) + " files, " + std::to_string(threads) + " threads");
      MemSourceTree source;
      source.AddDir("d");
      for (uint64_t i = 0; i < files; ++i) {
        source.AddFile("d/" + std::to_string(i), testing::RandomBytes(7000 + i, 100));
      }
      RoundTrip(source, threads, files);
    }
  }
}

TEST_F(BackupBatchTest, LargeFilesCloseBatchesByBytes) {
  // In listing order: two files that together reach the byte limit (the second closes the batch),
  // a small one that opens the next batch, and one at the limit by itself.
  const uint64_t limit = kFileBatchBytes;
  MemSourceTree source;
  source.AddFile("a", testing::RandomBytes(1, limit / 2 + 1));
  source.AddFile("b", testing::RandomBytes(2, limit / 2));
  source.AddFile("c", testing::RandomBytes(3, 1));
  source.AddFile("d", testing::RandomBytes(4, limit));
  RoundTrip(source, 4, 4);
}

}  // namespace
}  // namespace dds
