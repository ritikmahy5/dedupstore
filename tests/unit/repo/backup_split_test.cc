// Large files split across workers (D109): one worker chunks the file, every worker
// stores its segments, and the snapshot is the same as without splitting.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dds/fs/mem_fs.h"
#include "dds/repo/backup.h"
#include "dds/repo/backup_queue.h"
#include "dds/repo/file_store.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/restore_sink.h"
#include "dds/snapshot/source_tree.h"
#include "support/fake_process_identity.h"
#include "support/random_data.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::RandomBytes;

// A fresh MemFs repository of its own, so two backups can be compared from empty.
struct FreshRepo {
  FreshRepo() {
    EXPECT_TRUE(Repository::Init(fs, "/r", RepoParams{}).ok());
    auto opened = Repository::Open(fs, "/r", self);
    EXPECT_TRUE(opened.ok()) << opened.status().ToString();
    repo = std::move(opened).value();
  }
  Result<BackupResult> Backup(SourceTree& source, size_t threads, uint64_t split_bytes) {
    BackupOptions options;
    options.threads = threads;
    options.split_file_bytes = split_bytes;
    return repo->Backup(source, options);
  }
  MemTree Restore(const SnapshotId& id) {
    MemRestoreSink sink;
    EXPECT_TRUE(repo->Restore(id, sink, RestoreOptions{}).ok());
    return sink.nodes();
  }
  bool Verifies() {
    VerifyOptions deep;
    deep.deep = true;
    auto report = repo->Verify(deep);
    return report.ok() && report->ok();
  }
  MemFs fs;
  testing::FakeProcessIdentity self{"h", 1, 1};
  std::unique_ptr<Repository> repo;
};

std::vector<std::byte> Repeated(uint64_t seed, size_t block, int times) {
  const auto one = RandomBytes(seed, block);
  std::vector<std::byte> out;
  for (int i = 0; i < times; ++i) out.insert(out.end(), one.begin(), one.end());
  return out;
}

// Two large files (one repeating itself, so its chunks dedup across segments and workers), small
// files beside them, and a directory.
MemTree Mixed() {
  MemSourceTree t;
  t.AddFile("big", RandomBytes(1, 6 * kSegmentBytes + 12345));
  t.AddFile("repeats", Repeated(2, 300000, 12));
  t.AddDir("small");
  for (int i = 0; i < 20; ++i) {
    t.AddFile("small/" + std::to_string(i), RandomBytes(static_cast<uint64_t>(10 + i), 5000));
  }
  return t.nodes();
}

void ExpectSameSnapshot(const MemTree& tree, size_t threads, uint64_t split_bytes,
                        uint64_t expect_split) {
  MemSourceTree source(tree);
  FreshRepo whole, split;
  auto a = whole.Backup(source, threads, UINT64_MAX);
  auto b = split.Backup(source, threads, split_bytes);
  ASSERT_TRUE(a.ok()) << a.status().ToString();
  ASSERT_TRUE(b.ok()) << b.status().ToString();
  EXPECT_EQ(a->files_split, 0u);
  EXPECT_EQ(b->files_split, expect_split);
  EXPECT_EQ(a->manifest.root, b->manifest.root);
  EXPECT_EQ(a->manifest.logical_bytes, b->manifest.logical_bytes);
  // The same blobs are new, whichever worker stored them.
  EXPECT_EQ(a->ingest.blobs_written, b->ingest.blobs_written);
  EXPECT_EQ(a->ingest.bytes_written, b->ingest.bytes_written);
  EXPECT_TRUE(testing::DiffMemTrees(tree, split.Restore(b->id)).empty());
  EXPECT_TRUE(split.Verifies());
}

TEST(BackupSplitTest, SplittingDoesNotChangeTheSnapshot) {
  ExpectSameSnapshot(Mixed(), 4, kSegmentBytes, 2);
}

TEST(BackupSplitTest, ManyLargeFilesAtOnce) {
  MemSourceTree t;
  for (int i = 0; i < 12; ++i) {
    t.AddFile("f" + std::to_string(i),
              RandomBytes(static_cast<uint64_t>(100 + i), 2 * kSegmentBytes));
  }
  ExpectSameSnapshot(t.nodes(), 4, kSegmentBytes, 12);
}

TEST(BackupSplitTest, ASplitFileOfOneSegment) {
  MemSourceTree t;
  t.AddFile("f", RandomBytes(3, 40000));
  ExpectSameSnapshot(t.nodes(), 4, 1000, 1);
}

TEST(BackupSplitTest, OneWorkerNeverSplits) {
  MemSourceTree source(Mixed());
  FreshRepo r;
  auto b = r.Backup(source, 1, kSegmentBytes);
  ASSERT_TRUE(b.ok()) << b.status().ToString();
  EXPECT_EQ(b->files_split, 0u);
}

// A source whose files fail to read after `limit` bytes, or list one byte longer than they are.
class OddSourceTree : public SourceTree {
 public:
  OddSourceTree(MemTree nodes, uint64_t fail_after, bool list_longer)
      : inner_(std::move(nodes)), fail_after_(fail_after), list_longer_(list_longer) {}
  Result<std::vector<SourceEntry>> ListDir(const std::string& path) override {
    auto entries = inner_.ListDir(path);
    if (entries.ok() && list_longer_) {
      for (SourceEntry& e : *entries) e.size += 1;
    }
    return entries;
  }
  Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) override {
    auto inner = inner_.OpenFile(path);
    DDS_TRY(inner.status());
    return std::unique_ptr<ByteSource>(
        std::make_unique<FailingSource>(std::move(inner).value(), fail_after_));
  }
  Result<std::string> ReadLink(const std::string& path) override { return inner_.ReadLink(path); }

 private:
  class FailingSource : public ByteSource {
   public:
    FailingSource(std::unique_ptr<ByteSource> inner, uint64_t limit)
        : inner_(std::move(inner)), left_(limit) {}
    Result<size_t> Read(std::span<std::byte> out) override {
      if (left_ == 0) return Status::IoError("read failed (injected)");
      auto n = inner_->Read(out.first(std::min<uint64_t>(out.size(), left_)));
      if (n.ok()) left_ -= *n;
      return n;
    }

   private:
    std::unique_ptr<ByteSource> inner_;
    uint64_t left_;
  };
  MemSourceTree inner_;
  const uint64_t fail_after_;
  const bool list_longer_;
};

TEST(BackupSplitTest, AReadErrorInASplitFileFailsTheBackup) {
  MemSourceTree t;
  t.AddFile("big", RandomBytes(4, 8 * kSegmentBytes));
  OddSourceTree source(t.nodes(), 5 * kSegmentBytes, false);
  FreshRepo r;
  auto b = r.Backup(source, 4, kSegmentBytes);
  EXPECT_EQ(b.status().code(), StatusCode::kIoError) << b.status().ToString();
  EXPECT_NE(b.status().message().find("big"), std::string::npos) << b.status().ToString();
  EXPECT_TRUE(r.repo->ListSnapshots()->empty());
  MemSourceTree good(t.nodes());
  auto ok = r.Backup(good, 4, kSegmentBytes);
  ASSERT_TRUE(ok.ok()) << ok.status().ToString();
  EXPECT_TRUE(testing::DiffMemTrees(t.nodes(), r.Restore(ok->id)).empty());
}

TEST(BackupSplitTest, ASplitFileThatChangedSizeIsStoredAsRead) {
  MemSourceTree t;
  t.AddFile("big", RandomBytes(5, 3 * kSegmentBytes));
  OddSourceTree source(t.nodes(), UINT64_MAX, true);
  FreshRepo r;
  auto b = r.Backup(source, 4, kSegmentBytes);
  ASSERT_TRUE(b.ok()) << b.status().ToString();
  EXPECT_EQ(b->files_split, 1u);
  EXPECT_EQ(b->changed_while_reading, 1u);
  EXPECT_TRUE(testing::DiffMemTrees(t.nodes(), r.Restore(b->id)).empty());
}

// The queue's rules for segments, driven directly.
TEST(BackupQueueTest, ASegmentQueuesOnlyForAnIdleThread) {
  BackupQueue queue(1);
  Segment s;
  EXPECT_FALSE(queue.TryPushSegment(&s));  // nobody is waiting: the caller stores it
  std::optional<BackupQueue::Task> popped;
  std::thread worker([&] { popped = queue.Pop(); });
  while (!queue.TryPushSegment(&s)) std::this_thread::yield();  // once the worker waits in Pop
  worker.join();
  ASSERT_TRUE(popped.has_value());
  Segment** got = std::get_if<Segment*>(&*popped);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(*got, &s);
}

TEST(BackupQueueTest, AWaitingSplitterStoresQueuedSegmentsUntilItsOwnIsDone) {
  BackupQueue queue(1);
  Segment mine, other;
  std::atomic<bool> helped{false};
  std::thread splitter([&] {
    while (Segment* s = queue.NextWhileWaiting(&mine)) {
      EXPECT_EQ(s, &other);
      helped = true;
      queue.MarkDone(s);
    }
  });
  while (!queue.TryPushSegment(&other)) std::this_thread::yield();  // once the splitter waits
  while (!helped) std::this_thread::yield();  // it takes `other`, since `mine` is not done
  queue.MarkDone(&mine);
  splitter.join();  // and returns once `mine` is done
}

TEST(BackupQueueTest, CloseRefusesBatchesAndEndsPop) {
  BackupQueue queue(1);
  EXPECT_TRUE(queue.PushBatch(FileBatch{}));
  queue.Close();
  EXPECT_FALSE(queue.PushBatch(FileBatch{}));
  EXPECT_TRUE(queue.Pop().has_value());  // the batch queued before Close
  queue.BatchDone();
  EXPECT_FALSE(queue.Pop().has_value());
}

// The walk can end, and the queue close, before a worker reaches the large file in its batch: the
// other workers must stay until every batch taken is done, to store its segments.
TEST(BackupQueueTest, WorkersStayAfterCloseUntilEveryBatchIsDone) {
  BackupQueue queue(1);
  EXPECT_TRUE(queue.PushBatch(FileBatch{}));
  queue.Close();
  ASSERT_TRUE(queue.Pop().has_value());  // this thread now stores the batch
  Segment s;
  std::optional<BackupQueue::Task> first, second;
  std::thread worker([&] {
    first = queue.Pop();
    second = queue.Pop();
  });
  while (!queue.TryPushSegment(&s)) std::this_thread::yield();  // once the worker waits in Pop
  queue.BatchDone();
  worker.join();
  ASSERT_TRUE(first.has_value());
  EXPECT_NE(std::get_if<Segment*>(&*first), nullptr);
  EXPECT_FALSE(second.has_value());  // closed, empty, and every batch done
}

}  // namespace
}  // namespace dds
