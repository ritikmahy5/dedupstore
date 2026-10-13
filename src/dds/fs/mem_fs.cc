#include "dds/fs/mem_fs.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "dds/common/strings.h"

namespace dds {

namespace {

class MemWritableFile final : public WritableFile {
 public:
  explicit MemWritableFile(std::shared_ptr<MemFs::Inode> inode) : inode_(std::move(inode)) {}

  Status Append(std::span<const std::byte> data) override {
    MutexLock lock(inode_->mu);
    inode_->data.insert(inode_->data.end(), data.begin(), data.end());
    return Status::Ok();
  }
  Status Sync() override { return Status::Ok(); }
  Status Close() override { return Status::Ok(); }

 private:
  std::shared_ptr<MemFs::Inode> inode_;
};

class MemReadableFile final : public ReadableFile {
 public:
  MemReadableFile(std::shared_ptr<MemFs::Inode> inode, std::string path)
      : inode_(std::move(inode)), path_(std::move(path)) {}

  Status ReadAt(uint64_t offset, std::span<std::byte> out) const override {
    MutexLock lock(inode_->mu);
    const uint64_t size = inode_->data.size();
    if (offset > size || out.size() > size - offset) {
      return Status::IoError(StrCat(path_, ": read of ", out.size(), " bytes at offset ", offset,
                                    " extends past the end of the file (size ", size, ")"));
    }
    std::copy_n(inode_->data.begin() + static_cast<std::ptrdiff_t>(offset), out.size(),
                out.begin());
    return Status::Ok();
  }

  Result<uint64_t> Size() const override {
    MutexLock lock(inode_->mu);
    return static_cast<uint64_t>(inode_->data.size());
  }

 private:
  std::shared_ptr<MemFs::Inode> inode_;
  std::string path_;
};

}  // namespace

bool MemFs::IsDirLocked(const std::string& path) const {
  return path == "/" || dirs_.count(path) > 0;
}

bool MemFs::ParentExistsLocked(const std::string& path) const {
  return IsDirLocked(ParentDir(path));
}

Result<std::unique_ptr<WritableFile>> MemFs::CreateNew(const std::string& path) {
  MutexLock lock(mu_);
  if (files_.count(path) > 0 || IsDirLocked(path)) {
    return Status::AlreadyExists(StrCat("open ", path, ": file exists"));
  }
  if (!ParentExistsLocked(path)) {
    return Status::NotFound(StrCat("open ", path, ": no such directory"));
  }
  auto inode = std::make_shared<Inode>();
  files_[path] = inode;
  return std::unique_ptr<WritableFile>(std::make_unique<MemWritableFile>(std::move(inode)));
}

Result<std::unique_ptr<ReadableFile>> MemFs::OpenRead(const std::string& path) {
  MutexLock lock(mu_);
  auto it = files_.find(path);
  if (it == files_.end()) return Status::NotFound(StrCat("open ", path, ": no such file"));
  return std::unique_ptr<ReadableFile>(std::make_unique<MemReadableFile>(it->second, path));
}

Status MemFs::Rename(const std::string& from, const std::string& to) {
  MutexLock lock(mu_);
  auto it = files_.find(from);
  if (it == files_.end()) return Status::NotFound(StrCat("rename ", from, ": no such file"));
  if (IsDirLocked(to)) return Status::IoError(StrCat("rename ", from, ": target is a directory"));
  if (!ParentExistsLocked(to))
    return Status::NotFound(StrCat("rename ", to, ": no such directory"));
  std::shared_ptr<Inode> inode = it->second;
  files_.erase(it);
  files_[to] = std::move(inode);  // replaces any file at `to`, atomically under the lock
  return Status::Ok();
}

Status MemFs::Link(const std::string& existing, const std::string& link) {
  MutexLock lock(mu_);
  auto it = files_.find(existing);
  if (it == files_.end()) return Status::NotFound(StrCat("link ", existing, ": no such file"));
  if (files_.count(link) > 0 || IsDirLocked(link)) {
    return Status::AlreadyExists(StrCat("link ", link, ": file exists"));
  }
  if (!ParentExistsLocked(link))
    return Status::NotFound(StrCat("link ", link, ": no such directory"));
  files_[link] = it->second;
  return Status::Ok();
}

Status MemFs::Remove(const std::string& path) {
  MutexLock lock(mu_);
  if (IsDirLocked(path)) return Status::IoError(StrCat("unlink ", path, ": is a directory"));
  if (files_.erase(path) == 0) return Status::NotFound(StrCat("unlink ", path, ": no such file"));
  return Status::Ok();
}

Status MemFs::CreateDir(const std::string& path) {
  MutexLock lock(mu_);
  if (files_.count(path) > 0 || IsDirLocked(path)) {
    return Status::AlreadyExists(StrCat("mkdir ", path, ": file exists"));
  }
  if (!ParentExistsLocked(path))
    return Status::NotFound(StrCat("mkdir ", path, ": no such directory"));
  dirs_.insert(path);
  return Status::Ok();
}

Status MemFs::SyncDir(const std::string& dir) {
  MutexLock lock(mu_);
  if (!IsDirLocked(dir)) return Status::NotFound(StrCat("open ", dir, ": no such directory"));
  return Status::Ok();
}

Result<std::vector<std::string>> MemFs::ListDir(const std::string& dir) {
  MutexLock lock(mu_);
  if (!IsDirLocked(dir)) {
    if (files_.count(dir) > 0) return Status::IoError(StrCat("opendir ", dir, ": not a directory"));
    return Status::NotFound(StrCat("opendir ", dir, ": no such directory"));
  }
  // Both containers are sorted, so the entries under `dir` form one contiguous range in each.
  const std::string prefix = dir == "/" ? "/" : dir + "/";
  std::vector<std::string> names;
  auto add_if_child = [&](const std::string& path) {
    const std::string rest = path.substr(prefix.size());
    if (!rest.empty() && rest.find('/') == std::string::npos) names.push_back(rest);
  };
  for (auto it = files_.lower_bound(prefix); it != files_.end() && it->first.starts_with(prefix);
       ++it) {
    add_if_child(it->first);
  }
  for (auto it = dirs_.lower_bound(prefix); it != dirs_.end() && it->starts_with(prefix); ++it) {
    add_if_child(*it);
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<bool> MemFs::Exists(const std::string& path) {
  MutexLock lock(mu_);
  return files_.count(path) > 0 || IsDirLocked(path);
}

}  // namespace dds
