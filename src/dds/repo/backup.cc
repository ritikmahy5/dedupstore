#include "dds/repo/backup.h"

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

#include "dds/common/log.h"
#include "dds/common/strings.h"
#include "dds/repo/backup_parent.h"
#include "dds/repo/backup_queue.h"
#include "dds/repo/file_store.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/names.h"
#include "dds/snapshot/recipe.h"

namespace dds {

namespace {

// A directory as listed: its entries in name order. A file entry's size and pieces are filled in
// once the file has been stored; a directory entry's subtree once its child has been built.
struct DirNode {
  std::string path;
  std::vector<TreeEntry> entries;
  std::vector<std::unique_ptr<DirNode>> children;  // one per directory entry, in entry order
  // The parent snapshot's tree of the same directory, if any (D106). Queued file tasks point into
  // it, so it lives as long as the node.
  std::unique_ptr<Tree> parent;
};

// One backup's pipeline (ARCHITECTURE.md 4.1): the caller's thread walks and queues files in
// batches, worker threads store them, each through its own writer, and the caller's thread then
// builds the trees.
class Backup {
 public:
  Backup(RepoInternals& repo, SourceTree& source, IngestSession& session, BackupResult& result,
         BackupQueue& queue, int64_t parent_started_at_ns, const BackupOptions& options)
      : repo_(repo),
        source_(source),
        session_(session),
        result_(result),
        queue_(queue),
        parent_started_at_ns_(parent_started_at_ns),
        storer_(repo, source, session, queue, options.split_file_bytes, options.threads, stop_) {}

  // Lists `node.path` and everything below it into `node`, recording entries and queueing files.
  // Stops early, returning Aborted, once a worker has failed. Even then `node` keeps everything it
  // listed: queued tasks point into it, so it must outlive the workers (the caller owns it).
  Status Walk(DirNode& node);
  // Queues the walker's partial batch, if any. Call once, after Walk returned OK. False if the
  // queue was closed because a worker failed.
  bool FlushBatch();
  // A worker thread's loop: stores queued files, and segments of files other workers split,
  // through writer `writer` until the queue closes.
  void Work(size_t writer);
  // Stores the tree blob of `node` (children first) and returns its fingerprint.
  Result<Fingerprint> BuildTree(DirNode& node, size_t writer);

  // The first error any worker hit, or OK.
  Status worker_error() DDS_EXCLUDES(error_mu_);
  uint64_t files_stored() const { return files_.load(std::memory_order_relaxed); }
  uint64_t bytes_stored() const { return bytes_.load(std::memory_order_relaxed); }
  uint64_t files_reused() const { return reused_.load(std::memory_order_relaxed); }
  uint64_t bytes_reused() const { return reused_bytes_.load(std::memory_order_relaxed); }
  const FileStorer& storer() const { return storer_; }
  void AddWorkerTimings(BackupTimings& t) const {
    storer_.AddTimings(t);
    t.reuse_ns = reuse_ns_.load(std::memory_order_relaxed);
  }

 private:
  // Stores one file's batch entry: reused from the parent if it can be, read otherwise.
  void StoreTask(FileTask& task, size_t writer, std::vector<std::byte>& buffer,
                 std::optional<ReadGuard>& guard);
  // Reuses the parent's pieces for a file listed as unchanged since it (D106). False if a blob they
  // need is gone, and the file must be read.
  bool ReuseFile(FileTask& task);
  // The parent's entry for file `s` if it is unchanged since then, and the parent's tree of
  // subdirectory `name` (D106).
  const TreeEntry* UnchangedParent(const DirNode& node, const SourceEntry& s) const;
  std::unique_ptr<Tree> ParentSubtree(const DirNode& node, const std::string& name);
  // Records the first failure and stops the pipeline: the walker's next Push fails, and the
  // workers drain the queue without storing anything more.
  void Fail(const Status& status) DDS_EXCLUDES(error_mu_);
  // Adds a file to the walker's batch and queues the batch once it is full (D102). False if the
  // queue was closed because a worker failed.
  bool Enqueue(FileTask task);

  RepoInternals& repo_;
  SourceTree& source_;
  IngestSession& session_;
  BackupResult& result_;  // directory, symlink, and special counts: written by the walker only
  BackupQueue& queue_;
  // The batch being filled and its files' listed bytes: the walker's thread only.
  FileBatch batch_;
  uint64_t batch_bytes_ = 0;
  const int64_t parent_started_at_ns_;  // 0 without a parent

  std::atomic<bool> stop_{false};
  FileStorer storer_;  // after stop_, which it reads
  OrderedMutex error_mu_{lock_level::kLeaf, "backup.error"};
  Status first_error_ DDS_GUARDED_BY(error_mu_);
  // Relaxed: statistics, read only after every worker has been joined.
  std::atomic<uint64_t> files_{0};
  std::atomic<uint64_t> bytes_{0};
  std::atomic<uint64_t> reuse_ns_{0};
  std::atomic<uint64_t> reused_{0};
  std::atomic<uint64_t> reused_bytes_{0};
};

TreeEntry EntryFrom(const SourceEntry& s, EntryKind kind) {
  TreeEntry e;
  e.name = s.name;
  e.kind = kind;
  e.mode = s.mode;
  e.mtime_sec = s.mtime_sec;
  e.mtime_nsec = s.mtime_nsec;
  return e;
}

Status Backup::Walk(DirNode& node) {
  auto listing = source_.ListDir(node.path);
  DDS_TRY(listing.status());
  node.entries.reserve(listing->size());  // file tasks point into this vector: no reallocation
  for (const SourceEntry& s : *listing) {
    const std::string child = ChildPath(node.path, s.name);
    if (!IsValidName(s.name)) {
      return Status::InvalidArgument(StrCat(child, ": name cannot be stored (SPEC.md section 6)"));
    }
    switch (s.kind) {
      case SourceKind::kFile:
        node.entries.push_back(EntryFrom(s, EntryKind::kFile));
        if (!Enqueue({child, &node.entries.back(), s.size, UnchangedParent(node, s)})) {
          return Status::Aborted("backup stopped after a worker failed");
        }
        break;
      case SourceKind::kDirectory: {
        node.entries.push_back(EntryFrom(s, EntryKind::kDirectory));
        node.children.push_back(std::make_unique<DirNode>());
        node.children.back()->path = child;
        node.children.back()->parent = ParentSubtree(node, s.name);
        ++result_.manifest.directory_count;
        DDS_TRY(Walk(*node.children.back()));
        break;
      }
      case SourceKind::kSymlink: {
        auto target = source_.ReadLink(child);
        DDS_TRY(target.status());
        if (!IsValidSymlinkTarget(*target)) {
          return Status::InvalidArgument(StrCat(child, ": symlink target cannot be stored"));
        }
        node.entries.push_back(EntryFrom(s, EntryKind::kSymlink));
        node.entries.back().target = *target;
        ++result_.manifest.symlink_count;
        break;
      }
      case SourceKind::kOther:
        LogWarn(StrCat("skipping special file ", child));
        ++result_.skipped_special;
        break;
    }
  }
  return Status::Ok();
}

const TreeEntry* Backup::UnchangedParent(const DirNode& node, const SourceEntry& s) const {
  if (node.parent == nullptr) return nullptr;
  const TreeEntry* p = FindEntry(*node.parent, s.name);
  return p != nullptr && UnchangedSinceParent(s, *p, parent_started_at_ns_) ? p : nullptr;
}

std::unique_ptr<Tree> Backup::ParentSubtree(const DirNode& node, const std::string& name) {
  if (node.parent == nullptr) return nullptr;
  const TreeEntry* p = FindEntry(*node.parent, name);
  if (p == nullptr || p->kind != EntryKind::kDirectory) return nullptr;
  return ReadParentTree(repo_, p->subtree);
}

bool Backup::Enqueue(FileTask task) {
  batch_bytes_ += task.listed_size;
  batch_.push_back(std::move(task));
  if (batch_.size() < kFileBatchFiles && batch_bytes_ < kFileBatchBytes) return true;
  return FlushBatch();
}

bool Backup::FlushBatch() {
  if (batch_.empty()) return true;
  FileBatch full;
  full.swap(batch_);
  batch_bytes_ = 0;
  return queue_.PushBatch(std::move(full));
}

bool Backup::ReuseFile(FileTask& task) {
  const int64_t start = repo_.clock.MonotonicNanos();
  const bool reused = ReuseParentPieces(repo_, session_, *task.parent, *task.entry);
  reuse_ns_.fetch_add(static_cast<uint64_t>(repo_.clock.MonotonicNanos() - start),
                      std::memory_order_relaxed);
  if (!reused) return false;
  reused_.fetch_add(1, std::memory_order_relaxed);
  reused_bytes_.fetch_add(task.entry->size, std::memory_order_relaxed);
  return true;
}

void Backup::Fail(const Status& status) {
  {
    MutexLock lock(error_mu_);
    if (first_error_.ok()) first_error_ = status;
  }
  stop_ = true;
  queue_.Close();
}

Status Backup::worker_error() {
  MutexLock lock(error_mu_);
  return first_error_;
}

void Backup::StoreTask(FileTask& task, size_t writer, std::vector<std::byte>& buffer,
                       std::optional<ReadGuard>& guard) {
  if (task.parent != nullptr) {
    if (!guard.has_value()) guard.emplace(repo_.epochs);
    if (ReuseFile(task)) {
      files_.fetch_add(1, std::memory_order_relaxed);
      bytes_.fetch_add(task.entry->size, std::memory_order_relaxed);
      return;
    }
  }
  const Status stored = storer_.StoreFile(task, writer, buffer);
  if (!stored.ok()) {
    Fail(stored);
    return;
  }
  files_.fetch_add(1, std::memory_order_relaxed);
  bytes_.fetch_add(task.entry->size, std::memory_order_relaxed);
}

void Backup::Work(size_t writer) {
  std::vector<std::byte> buffer;  // this worker's read buffer, sized by the first file's reader
  while (auto task = queue_.Pop()) {
    if (auto* segment = std::get_if<Segment*>(&*task)) {
      storer_.StoreQueuedSegment(**segment, writer);  // a splitting worker waits for it
      continue;
    }
    // One read guard for all of a batch's reused files, entered at the first: a guard per file
    // made every worker take the epoch mutex twice per file, and at 16 threads they waited on it
    // (S5parent, D106). Holding it longer only delays reclaiming containers GC retires meanwhile.
    std::optional<ReadGuard> guard;
    auto* batch = std::get_if<FileBatch>(&*task);
    DDS_CHECK(batch != nullptr);  // a task is a segment or a batch
    for (FileTask& task_in_batch : *batch) {
      if (stop_) break;  // after a failure, drain without storing
      StoreTask(task_in_batch, writer, buffer, guard);
    }
    queue_.BatchDone();
  }
}

Result<Fingerprint> Backup::BuildTree(DirNode& node, size_t writer) {
  size_t child = 0;
  Tree tree;
  for (TreeEntry& e : node.entries) {
    if (e.kind == EntryKind::kDirectory) {
      auto fp = BuildTree(*node.children[child++], writer);
      DDS_TRY(fp.status());
      e.subtree = *fp;
    }
  }
  tree.entries = std::move(node.entries);
  auto bytes = EncodeTree(tree);
  if (!bytes.ok()) {
    return bytes.status().WithContext(node.path.empty() ? "the backup root" : node.path);
  }
  const Fingerprint fp = FingerprintOf(BlobType::kTree, *bytes);
  DDS_TRY(storer_.PutIfNew(fp, BlobType::kTree, *bytes, writer));
  return fp;
}

// The parent snapshot's start time and root tree (D106), or 0 and nothing without a parent.
Result<int64_t> LoadParent(RepoInternals& repo, const BackupOptions& options, DirNode& root) {
  if (!options.parent.has_value()) return int64_t{0};
  auto parent = repo.snapshots.Read(*options.parent);
  DDS_TRY(parent.status());
  root.parent = ReadParentTree(repo, parent->root);
  return parent->started_at_ns;
}

}  // namespace

Result<BackupResult> RunBackup(RepoInternals& repo, SourceTree& source,
                               const BackupOptions& options, const Clock& clock) {
  // Checked before any work, rather than when the manifest is built at the end.
  if (options.tag.size() > kMaxTagBytes) {
    return Status::InvalidArgument(
        StrCat("tag is ", options.tag.size(), " bytes; the limit is ", kMaxTagBytes));
  }
  if (options.threads == 0) return Status::InvalidArgument("a backup needs at least one thread");
  const size_t workers = options.threads;
  // Before anything is listed: a later backup skips a file only if its mtime is older than this.
  const int64_t started_at_ns = clock.WallNanos();
  BackupResult result;
  // Writers 0..workers-1 belong to the workers, one each (D31); writer `workers` to the trees.
  IngestSession session(repo, workers + 1);
  // 4 x workers batches: enough queued files that no worker waits for the walker, few enough that
  // the queue's memory stays small however big the tree is (4.1, D102).
  BackupQueue queue(4 * workers);
  // Workers write into entries of this tree until they are joined, so it lives in this scope and
  // nothing returns before the join: a failed walk must not free entries a worker is filling.
  DirNode root;
  auto parent_started_at_ns = LoadParent(repo, options, root);
  DDS_TRY(parent_started_at_ns.status());
  Backup backup(repo, source, session, result, queue, *parent_started_at_ns, options);
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (size_t i = 0; i < workers; ++i) threads.emplace_back([&backup, i] { backup.Work(i); });
  const int64_t walk_start = clock.MonotonicNanos();
  Status walked = backup.Walk(root);
  // The last, partial batch. If a worker failed meanwhile, the queue is closed and its error wins.
  if (walked.ok() && !backup.FlushBatch()) {
    walked = Status::Aborted("backup stopped after a worker failed");
  }
  result.timings.walk_ns = static_cast<uint64_t>(clock.MonotonicNanos() - walk_start);
  queue.Close();
  for (std::thread& t : threads) t.join();
  backup.AddWorkerTimings(result.timings);
  DDS_TRY(backup.worker_error());  // a worker's error explains an Aborted walk
  DDS_TRY(walked);
  result.manifest.file_count = backup.files_stored();
  result.manifest.logical_bytes = backup.bytes_stored();
  result.changed_while_reading = backup.storer().changed_while_reading();
  result.files_split = backup.storer().files_split();
  result.segments_queued = backup.storer().segments_queued();
  result.parent = options.parent;
  result.files_reused = backup.files_reused();
  result.bytes_reused = backup.bytes_reused();
  const int64_t trees_start = clock.MonotonicNanos();
  auto root_fp = backup.BuildTree(root, workers);
  DDS_TRY(root_fp.status());
  result.timings.trees_ns = static_cast<uint64_t>(clock.MonotonicNanos() - trees_start);

  Manifest& m = result.manifest;
  m.origin_repo_id = repo.config.repo_id;
  m.created_at_ns = clock.WallNanos();
  m.started_at_ns = started_at_ns;
  m.root = *root_fp;
  // Informational strings: a longer host name or path (not possible on Linux or macOS, whose limits
  // are lower) is cut rather than failing the backup.
  m.host = options.host.substr(0, kMaxHostBytes);
  m.source_path = options.source_path.substr(0, kMaxSourcePathBytes);
  m.tag = options.tag;
  DDS_TRY(ValidateManifest(m));
  const int64_t commit_start = clock.MonotonicNanos();
  auto id = session.Commit(EncodeManifest(m));
  DDS_TRY(id.status());
  result.timings.commit_ns = static_cast<uint64_t>(clock.MonotonicNanos() - commit_start);
  result.id = *id;
  result.ingest = session.counters();
  return result;
}

}  // namespace dds
