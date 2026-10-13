#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"
#include "dds/fs/fs.h"

namespace dds {

// An in-memory Fs that models PosixFs: O_EXCL creation, atomic rename-replace, link failing on an
// existing name, and handles that stay readable after their name is removed (files are shared
// inodes). Sync and SyncDir succeed and do nothing, because nothing is lost without a crash;
// CrashSimFs models crashes. Used by fast tests and by the `mem` benchmark mode, which measures
// CPU cost without storage I/O.
//
// The root directory "/" always exists.
class MemFs final : public Fs {
 public:
  MemFs() = default;

  Result<std::unique_ptr<WritableFile>> CreateNew(const std::string& path) override;
  Result<std::unique_ptr<ReadableFile>> OpenRead(const std::string& path) override;
  Status Rename(const std::string& from, const std::string& to) override;
  Status Link(const std::string& existing, const std::string& link) override;
  Status Remove(const std::string& path) override;
  Status CreateDir(const std::string& path) override;
  Status SyncDir(const std::string& dir) override;
  Result<std::vector<std::string>> ListDir(const std::string& dir) override;
  Result<bool> Exists(const std::string& path) override;

  // File contents, shared between every name and handle of one file. Each inode has its own
  // mutex so that writers of different files do not serialize on the namespace lock.
  struct Inode {
    OrderedMutex mu{lock_level::kLeaf, "memfs.inode"};
    std::vector<std::byte> data DDS_GUARDED_BY(mu);
  };

 private:
  // True if `path` names an existing directory ("/" always does).
  bool IsDirLocked(const std::string& path) const DDS_REQUIRES(mu_);
  bool ParentExistsLocked(const std::string& path) const DDS_REQUIRES(mu_);

  // The namespace. Never held while an inode mutex is taken: both are leaf locks.
  mutable OrderedMutex mu_{lock_level::kLeaf, "memfs.namespace"};
  std::map<std::string, std::shared_ptr<Inode>> files_ DDS_GUARDED_BY(mu_);
  std::set<std::string> dirs_ DDS_GUARDED_BY(mu_);
};

}  // namespace dds
