#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/status.h"
#include "dds/common/thread_annotations.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/tree.h"

namespace dds {

// A regular file to store, pointing at its entry in the listed tree. The walker creates the entry
// and never touches it again; one worker fills in its size and pieces; the tree builder reads it
// after the workers are joined, which orders the worker's writes before the reads.
struct FileTask {
  std::string path;
  TreeEntry* entry;
  uint64_t listed_size;
  const TreeEntry* parent;  // the parent's entry if the file is unchanged since it (D106), or null
};

// Files queued together (D102).
using FileBatch = std::vector<FileTask>;

// Consecutive whole chunks of one large file, which any worker may fingerprint and store (D109).
// Ownership passes by hand: the worker splitting the file fills `data` and `lengths` and
// queues it; the worker that takes it fills `refs` and `status` and marks it done; the splitting
// worker reads them after seeing it done. The queue's mutex orders each hand-off.
struct Segment {
  std::vector<std::byte> data;    // the chunks' bytes, back to back
  std::vector<uint32_t> lengths;  // each chunk's length, in file order
  std::vector<ChunkRef> refs;     // once stored: each chunk's fingerprint and length
  Status status;                  // once stored
};

// A backup's work (ARCHITECTURE.md 4.1): batches of files from the walker, and segments of large
// files from the workers splitting them (D109). A worker takes a waiting segment before any batch,
// because a splitting worker is waiting for it. Close() ends the batches: later PushBatch calls
// fail, and Pop returns what is left, then nullopt once no batch is still being stored, so idle
// workers stay to help with a file split after the walk ended. The mutex is a leaf (5.3); nothing
// else is locked while it is held.
class BackupQueue {
 public:
  using Task = std::variant<FileBatch, Segment*>;

  explicit BackupQueue(size_t batch_capacity);
  BackupQueue(const BackupQueue&) = delete;
  BackupQueue& operator=(const BackupQueue&) = delete;

  // The walker: blocks while `batch_capacity` batches wait. False, and `batch` dropped, once
  // closed.
  bool PushBatch(FileBatch batch) DDS_EXCLUDES(mu_);
  // A splitting worker: queues `s` only if more threads are waiting for work than segments are
  // queued; otherwise false, and the caller stores `s` itself.
  // Never blocks, so a worker never waits for room that only workers make, and segments queue only
  // for idle threads: when every worker is busy, as with many large files at once, splitting costs
  // no extra memory.
  bool TryPushSegment(Segment* s) DDS_EXCLUDES(mu_);
  // A worker: the next task, or nullopt once the queue is closed and empty and every batch taken is
  // done. A worker calls BatchDone after storing each batch it took.
  std::optional<Task> Pop() DDS_EXCLUDES(mu_);
  void BatchDone() DDS_EXCLUDES(mu_);
  // Whether a segment queued now would find an idle thread. A hint for a splitting worker deciding
  // whether to copy chunks into a segment at all; TryPushSegment decides.
  bool HasIdleThread() DDS_EXCLUDES(mu_);
  // A splitting worker waiting for its segment `mine`: blocks until `mine` is done (nullptr) or a
  // segment is queued (returned: the caller stores it, marks it done, and calls again). Helping
  // instead of only waiting means a queued segment never depends on a worker that has exited.
  Segment* NextWhileWaiting(const Segment* mine) DDS_EXCLUDES(mu_);
  // Whoever stored `s`: its refs and status are final.
  void MarkDone(const Segment* s) DDS_EXCLUDES(mu_);
  void Close() DDS_EXCLUDES(mu_);

 private:
  const size_t batch_capacity_;
  OrderedMutex mu_{lock_level::kLeaf, "backup.queue"};
  // All waited on with mu_. Separate, so each event wakes only the threads it concerns: a batch
  // wakes one idle worker, as before splitting existed (D102), not every thread.
  std::condition_variable_any work_;  // idle workers in Pop: a batch, segment, batch done, close
  std::condition_variable_any splitters_;  // splitters in NextWhileWaiting: a segment or one done
  std::condition_variable_any room_;       // the walker in PushBatch: a batch taken, or close
  std::deque<FileBatch> batches_ DDS_GUARDED_BY(mu_);
  std::deque<Segment*> segments_ DDS_GUARDED_BY(mu_);
  std::unordered_set<const Segment*> done_ DDS_GUARDED_BY(mu_);  // stored, not yet waited for
  size_t waiting_ DDS_GUARDED_BY(mu_) = 0;  // threads in Pop or NextWhileWaiting, idle
  size_t active_ DDS_GUARDED_BY(mu_) = 0;   // batches taken and not yet done
  bool closed_ DDS_GUARDED_BY(mu_) = false;
};

}  // namespace dds
