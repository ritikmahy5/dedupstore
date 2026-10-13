#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"
#include "dds/fs/fs.h"
#include "dds/fs/mem_fs.h"

namespace dds {

// An Fs for crash testing (TESTING.md 3.6, D76). It behaves exactly like MemFs, which it wraps
// (the conformance suite holds it to the same semantics), and records what a power loss could
// undo under assumptions A1-A3 (ARCHITECTURE.md 7.1):
// - per file: the bytes appended, and how many of them the last Sync made durable (A1);
// - per directory: the creates, links, renames into it, removes, and mkdirs since its last
//   SyncDir (A2). A rename is one operation, made durable by SyncDir of its target directory (A3).
//
// CrashAfter(n) makes every call after the n-th mutating call fail with IoError, which stops the
// process logic at that point as a kill would. Materialize(seed) then builds one post-crash disk:
// durable state, plus each pending directory operation surviving or not, plus each file keeping a
// prefix of its unsynced bytes. Seed 0 keeps nothing pending and seed 1 keeps everything, so both
// extremes are always explored; other seeds choose at random.
//
// Mutating calls: CreateNew, Rename, Link, Remove, CreateDir, SyncDir, and WritableFile::Append and
// Sync. Thread-safe: one mutex orders every call, so the recorded order is the order of effect.
class CrashSimFs final : public Fs {
 public:
  CrashSimFs();
  ~CrashSimFs() override;

  Result<std::unique_ptr<WritableFile>> CreateNew(const std::string& path) override;
  Result<std::unique_ptr<ReadableFile>> OpenRead(const std::string& path) override;
  Status Rename(const std::string& from, const std::string& to) override;
  Status Link(const std::string& existing, const std::string& link) override;
  Status Remove(const std::string& path) override;
  Status CreateDir(const std::string& path) override;
  Status SyncDir(const std::string& dir) override;
  Result<std::vector<std::string>> ListDir(const std::string& dir) override;
  Result<bool> Exists(const std::string& path) override;

  // After the n-th mutating call (counted from construction), every call fails with IoError.
  // Read handles opened earlier keep working: reads cannot change the disk.
  void CrashAfter(uint64_t n) DDS_EXCLUDES(mu_);
  // Mutating calls attempted so far, including ones refused after the crash point.
  uint64_t mutating_calls() const DDS_EXCLUDES(mu_);
  bool crashed() const DDS_EXCLUDES(mu_);

  // One disk that A1-A3 allow if power were lost now.
  std::unique_ptr<MemFs> Materialize(uint64_t seed) const DDS_EXCLUDES(mu_);

 private:
  friend class CrashSimWritableFile;

  struct Inode {
    std::vector<std::byte> data;  // everything appended
    size_t synced = 0;            // data[0, synced) is durable (A1)
  };
  enum class OpKind { kMkdir, kBind, kRemove, kRename };
  struct DirOp {
    OpKind kind;
    std::string owner;             // the directory whose SyncDir makes this operation durable
    std::string path;              // the name made, bound, removed, or renamed to
    std::string from;              // kRename only: the old name
    std::shared_ptr<Inode> inode;  // null for kMkdir
  };
  // Directories and names, by inode, as they would be found on disk.
  struct Namespace {
    std::set<std::string> dirs;
    std::map<std::string, std::shared_ptr<Inode>> names;
    void Apply(const DirOp& op);
  };

  // Counts one mutating call. IoError if it falls after the crash point.
  Status BeginMutation(const char* op, const std::string& path) DDS_REQUIRES(mu_);
  Status CheckNotCrashed(const char* op, const std::string& path) const DDS_REQUIRES(mu_);
  void Record(DirOp op) DDS_REQUIRES(mu_) { pending_.push_back(std::move(op)); }
  void MakeDurable(const std::string& dir) DDS_REQUIRES(mu_);

  MemFs live_;
  mutable OrderedMutex mu_{lock_level::kFsModel, "crash_sim"};
  // Inode fields are guarded by mu_ too; only this class and its file handles touch them.
  std::map<std::string, std::shared_ptr<Inode>> live_names_ DDS_GUARDED_BY(mu_);
  Namespace durable_ DDS_GUARDED_BY(mu_);
  std::vector<DirOp> pending_ DDS_GUARDED_BY(mu_);  // in the order they took effect
  uint64_t calls_ DDS_GUARDED_BY(mu_) = 0;
  uint64_t crash_after_ DDS_GUARDED_BY(mu_) = std::numeric_limits<uint64_t>::max();
  bool crashed_ DDS_GUARDED_BY(mu_) = false;
};

}  // namespace dds
