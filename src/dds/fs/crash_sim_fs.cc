#include "dds/fs/crash_sim_fs.h"

#include <algorithm>

#include "dds/common/check.h"
#include "dds/common/splitmix64.h"
#include "dds/common/strings.h"

namespace dds {

class CrashSimWritableFile final : public WritableFile {
 public:
  CrashSimWritableFile(CrashSimFs& fs, std::unique_ptr<WritableFile> live, std::string path,
                       std::shared_ptr<CrashSimFs::Inode> inode)
      : fs_(fs), live_(std::move(live)), path_(std::move(path)), inode_(std::move(inode)) {}

  Status Append(std::span<const std::byte> data) override {
    MutexLock lock(fs_.mu_);
    DDS_TRY(fs_.BeginMutation("append", path_));
    DDS_TRY(live_->Append(data));
    inode_->data.insert(inode_->data.end(), data.begin(), data.end());
    return Status::Ok();
  }

  Status Sync() override {
    MutexLock lock(fs_.mu_);
    DDS_TRY(fs_.BeginMutation("fsync", path_));
    DDS_TRY(live_->Sync());
    inode_->synced = inode_->data.size();  // A1
    return Status::Ok();
  }

  Status Close() override {
    MutexLock lock(fs_.mu_);
    DDS_TRY(fs_.CheckNotCrashed("close", path_));
    return live_->Close();
  }

 private:
  CrashSimFs& fs_;
  std::unique_ptr<WritableFile> live_;
  const std::string path_;
  std::shared_ptr<CrashSimFs::Inode> inode_;
};

CrashSimFs::CrashSimFs() {
  MutexLock lock(mu_);
  durable_.dirs.insert("/");
}

CrashSimFs::~CrashSimFs() = default;

void CrashSimFs::Namespace::Apply(const DirOp& op) {
  // No check that the parent directory exists: a directory's contents can be durable before its
  // own name is (SyncDir of the child before the parent). Materialize drops whatever ends up
  // unreachable.
  switch (op.kind) {
    case OpKind::kMkdir:
      dirs.insert(op.path);
      break;
    case OpKind::kBind:
      names[op.path] = op.inode;
      break;
    case OpKind::kRemove: {
      auto it = names.find(op.path);
      if (it != names.end() && it->second == op.inode) names.erase(it);
      break;
    }
    case OpKind::kRename: {
      names[op.path] = op.inode;
      auto it = names.find(op.from);
      if (op.from != op.path && it != names.end() && it->second == op.inode) names.erase(it);
      break;
    }
  }
}

Status CrashSimFs::CheckNotCrashed(const char* op, const std::string& path) const {
  if (crashed_) return Status::IoError(StrCat(op, " ", path, ": simulated crash"));
  return Status::Ok();
}

Status CrashSimFs::BeginMutation(const char* op, const std::string& path) {
  ++calls_;
  if (calls_ > crash_after_) crashed_ = true;
  return CheckNotCrashed(op, path);
}

void CrashSimFs::MakeDurable(const std::string& dir) {
  // A2: SyncDir(dir) makes durable every operation whose effect lands in dir, in order. A rename
  // into dir also retires earlier pending binds of its old name to the same inode: the old name
  // no longer refers to that file, and the rename is now durable, so it must never reappear (A3).
  std::vector<bool> drop(pending_.size(), false);
  for (size_t i = 0; i < pending_.size(); ++i) {
    const DirOp& op = pending_[i];
    if (op.owner != dir) continue;
    durable_.Apply(op);
    drop[i] = true;
    if (op.kind != OpKind::kRename) continue;
    for (size_t j = 0; j < i; ++j) {
      const DirOp& earlier = pending_[j];
      const bool binds_old_name =
          (earlier.kind == OpKind::kBind || earlier.kind == OpKind::kRename) &&
          earlier.path == op.from && earlier.inode == op.inode;
      if (binds_old_name) drop[j] = true;
    }
  }
  std::vector<DirOp> kept;
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!drop[i]) kept.push_back(std::move(pending_[i]));
  }
  pending_ = std::move(kept);
}

Result<std::unique_ptr<WritableFile>> CrashSimFs::CreateNew(const std::string& path) {
  MutexLock lock(mu_);
  DDS_TRY(BeginMutation("open", path));
  auto file = live_.CreateNew(path);
  DDS_TRY(file.status());
  auto inode = std::make_shared<Inode>();
  live_names_[path] = inode;
  Record({OpKind::kBind, ParentDir(path), path, "", inode});
  return std::unique_ptr<WritableFile>(
      std::make_unique<CrashSimWritableFile>(*this, std::move(file).value(), path, inode));
}

Result<std::unique_ptr<ReadableFile>> CrashSimFs::OpenRead(const std::string& path) {
  MutexLock lock(mu_);
  DDS_TRY(CheckNotCrashed("open", path));
  return live_.OpenRead(path);
}

Status CrashSimFs::Rename(const std::string& from, const std::string& to) {
  MutexLock lock(mu_);
  DDS_TRY(BeginMutation("rename", from));
  DDS_TRY(live_.Rename(from, to));
  auto inode = live_names_.at(from);
  live_names_.erase(from);
  live_names_[to] = inode;
  Record({OpKind::kRename, ParentDir(to), to, from, inode});
  return Status::Ok();
}

Status CrashSimFs::Link(const std::string& existing, const std::string& link) {
  MutexLock lock(mu_);
  DDS_TRY(BeginMutation("link", link));
  DDS_TRY(live_.Link(existing, link));
  auto inode = live_names_.at(existing);
  live_names_[link] = inode;
  Record({OpKind::kBind, ParentDir(link), link, "", inode});
  return Status::Ok();
}

Status CrashSimFs::Remove(const std::string& path) {
  MutexLock lock(mu_);
  DDS_TRY(BeginMutation("unlink", path));
  DDS_TRY(live_.Remove(path));
  auto inode = live_names_.at(path);
  live_names_.erase(path);
  Record({OpKind::kRemove, ParentDir(path), path, "", inode});
  return Status::Ok();
}

Status CrashSimFs::CreateDir(const std::string& path) {
  MutexLock lock(mu_);
  DDS_TRY(BeginMutation("mkdir", path));
  DDS_TRY(live_.CreateDir(path));
  Record({OpKind::kMkdir, ParentDir(path), path, "", nullptr});
  return Status::Ok();
}

Status CrashSimFs::SyncDir(const std::string& dir) {
  MutexLock lock(mu_);
  DDS_TRY(BeginMutation("fsync", dir));
  DDS_TRY(live_.SyncDir(dir));
  MakeDurable(dir);
  return Status::Ok();
}

Result<std::vector<std::string>> CrashSimFs::ListDir(const std::string& dir) {
  MutexLock lock(mu_);
  DDS_TRY(CheckNotCrashed("opendir", dir));
  return live_.ListDir(dir);
}

Result<bool> CrashSimFs::Exists(const std::string& path) {
  MutexLock lock(mu_);
  DDS_TRY(CheckNotCrashed("stat", path));
  return live_.Exists(path);
}

void CrashSimFs::CrashAfter(uint64_t n) {
  MutexLock lock(mu_);
  crash_after_ = n;
}

uint64_t CrashSimFs::mutating_calls() const {
  MutexLock lock(mu_);
  return calls_;
}

bool CrashSimFs::crashed() const {
  MutexLock lock(mu_);
  return crashed_;
}

std::unique_ptr<MemFs> CrashSimFs::Materialize(uint64_t seed) const {
  MutexLock lock(mu_);
  uint64_t rng_state = seed;
  const auto rng = [&] { return SplitMix64Next(rng_state); };
  const auto survives = [&] { return seed == 1 || (seed != 0 && (rng() & 1) == 1); };
  Namespace disk = durable_;
  for (const DirOp& op : pending_) {
    if (survives()) disk.Apply(op);
  }
  auto out = std::make_unique<MemFs>();
  // Only what is reachable from "/" exists after the crash. std::set order puts every directory
  // after its parent (a parent's path is a prefix), so one pass decides reachability.
  std::set<std::string> reachable = {"/"};
  for (const std::string& dir : disk.dirs) {
    if (dir == "/" || reachable.count(ParentDir(dir)) == 0) continue;
    reachable.insert(dir);
    DDS_CHECK(out->CreateDir(dir).ok());
  }
  // Each file keeps its synced bytes plus a prefix of the rest (A1 promises nothing more), chosen
  // once per file even if several names refer to it.
  std::map<const Inode*, std::string> first_name;
  for (const auto& [name, inode] : disk.names) {
    if (reachable.count(ParentDir(name)) == 0) continue;
    auto [it, first] = first_name.emplace(inode.get(), name);
    if (!first) {
      DDS_CHECK(out->Link(it->second, name).ok());
      continue;
    }
    const size_t unsynced = inode->data.size() - inode->synced;
    const size_t keep = seed == 0 ? 0 : seed == 1 ? unsynced : rng() % (unsynced + 1);
    auto file = out->CreateNew(name);
    DDS_CHECK(file.ok());
    DDS_CHECK((*file)->Append(std::span(inode->data).first(inode->synced + keep)).ok());
    DDS_CHECK((*file)->Close().ok());
  }
  return out;
}

}  // namespace dds
