#include "dds/snapshot/restore_sink.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "dds/common/strings.h"
#include "dds/fs/posix_fs.h"
#include "dds/snapshot/names.h"

namespace dds {

namespace {

Status CheckName(std::string_view name) {
  if (!IsValidName(name)) return Status::InvalidArgument(StrCat("invalid entry name '", name, "'"));
  return Status::Ok();
}

// Both times set to the recorded mtime: only the modification time is part of the snapshot.
std::array<struct timespec, 2> Times(const EntryMeta& meta) {
  struct timespec t{};
  t.tv_sec = static_cast<time_t>(meta.mtime_sec);
  t.tv_nsec = static_cast<decltype(t.tv_nsec)>(meta.mtime_nsec);
  return {t, t};
}

class PosixFileSink final : public FileSink {
 public:
  PosixFileSink(UniqueFd fd, std::string path) : fd_(std::move(fd)), path_(std::move(path)) {}

  Status Write(std::span<const std::byte> data) override {
    while (!data.empty()) {
      const ssize_t n = ::write(fd_.get(), data.data(), data.size());
      if (n < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus(errno, "write", path_);
      }
      data = data.subspan(static_cast<size_t>(n));
    }
    return Status::Ok();
  }

  Status Finish(const EntryMeta& meta) override {
    if (::fchmod(fd_.get(), static_cast<mode_t>(meta.mode)) != 0) {
      return ErrnoStatus(errno, "chmod", path_);
    }
    const auto times = Times(meta);
    if (::futimens(fd_.get(), times.data()) != 0) return ErrnoStatus(errno, "futimens", path_);
    return fd_.Close().WithContext(path_);
  }

 private:
  UniqueFd fd_;
  std::string path_;
};

class MemFileSink final : public FileSink {
 public:
  explicit MemFileSink(MemNode* node) : node_(node) {}
  Status Write(std::span<const std::byte> data) override {
    node_->content.insert(node_->content.end(), data.begin(), data.end());
    return Status::Ok();
  }
  Status Finish(const EntryMeta& meta) override {
    node_->mode = meta.mode;
    node_->mtime_sec = meta.mtime_sec;
    node_->mtime_nsec = meta.mtime_nsec;
    return Status::Ok();
  }

 private:
  MemNode* node_;  // std::map nodes are stable while the sink lives
};

}  // namespace

Result<PosixRestoreSink::OpenDir*> PosixRestoreSink::Lookup(DirHandle dir) {
  auto it = dirs_.find(dir);
  if (it == dirs_.end()) return Status::InvalidArgument(StrCat("unknown directory handle ", dir));
  return &it->second;
}

Result<RestoreSink::DirHandle> PosixRestoreSink::OpenRoot() {
  // The destination path itself may be a symlink the user chose (for example macOS's /tmp); only
  // entries inside it are created with O_NOFOLLOW.
  if (::mkdir(dest_.c_str(), 0700) != 0) {
    if (errno != EEXIST) return ErrnoStatus(errno, "mkdir", dest_);
    std::unique_ptr<DIR, int (*)(DIR*)> d(::opendir(dest_.c_str()), &::closedir);
    if (d == nullptr) return Status::AlreadyExists(StrCat(dest_, " exists and is not a directory"));
    for (const struct dirent* e = ::readdir(d.get()); e != nullptr; e = ::readdir(d.get())) {
      const std::string_view name = e->d_name;
      if (name != "." && name != "..") {
        return Status::AlreadyExists(StrCat(dest_, " is not empty"));
      }
    }
  }
  UniqueFd fd(::open(dest_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!fd.valid()) return ErrnoStatus(errno, "open", dest_);
  const DirHandle handle = next_handle_++;
  dirs_.emplace(handle, OpenDir{std::move(fd), dest_});
  return handle;
}

Result<RestoreSink::DirHandle> PosixRestoreSink::MakeDir(DirHandle parent, std::string_view name) {
  DDS_TRY(CheckName(name));
  auto p = Lookup(parent);
  DDS_TRY(p.status());
  const std::string n(name);
  const std::string path = (*p)->path + "/" + n;
  // mkdirat never follows or replaces: EEXIST if any entry, a symlink included, has the name.
  if (::mkdirat((*p)->fd.get(), n.c_str(), 0700) != 0) return ErrnoStatus(errno, "mkdir", path);
  UniqueFd fd(::openat((*p)->fd.get(), n.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!fd.valid()) return ErrnoStatus(errno, "open", path);
  const DirHandle handle = next_handle_++;
  dirs_.emplace(handle, OpenDir{std::move(fd), path});
  return handle;
}

Result<std::unique_ptr<FileSink>> PosixRestoreSink::CreateFile(DirHandle parent,
                                                               std::string_view name) {
  DDS_TRY(CheckName(name));
  auto p = Lookup(parent);
  DDS_TRY(p.status());
  const std::string n(name);
  const std::string path = (*p)->path + "/" + n;
  // O_EXCL: never replace; O_NOFOLLOW: never write through a symlink, even one restored earlier.
  UniqueFd fd(::openat((*p)->fd.get(), n.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!fd.valid()) return ErrnoStatus(errno, "create", path);
  return std::unique_ptr<FileSink>(std::make_unique<PosixFileSink>(std::move(fd), path));
}

Status PosixRestoreSink::MakeSymlink(DirHandle parent, std::string_view name,
                                     std::string_view target, const EntryMeta& meta) {
  DDS_TRY(CheckName(name));
  auto p = Lookup(parent);
  DDS_TRY(p.status());
  const std::string n(name);
  const std::string t(target);
  const std::string path = (*p)->path + "/" + n;
  if (::symlinkat(t.c_str(), (*p)->fd.get(), n.c_str()) != 0) {
    return ErrnoStatus(errno, "symlink", path);
  }
  // Linux cannot change a symlink's mode, so only its times are restored (D54).
  const auto times = Times(meta);
  if (::utimensat((*p)->fd.get(), n.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) != 0) {
    return ErrnoStatus(errno, "utimensat", path);
  }
  return Status::Ok();
}

Status PosixRestoreSink::FinishDir(DirHandle dir, const std::optional<EntryMeta>& meta) {
  auto d = Lookup(dir);
  DDS_TRY(d.status());
  if (meta.has_value()) {
    if (::fchmod((*d)->fd.get(), static_cast<mode_t>(meta->mode)) != 0) {
      return ErrnoStatus(errno, "chmod", (*d)->path);
    }
    const auto times = Times(*meta);
    if (::futimens((*d)->fd.get(), times.data()) != 0) {
      return ErrnoStatus(errno, "futimens", (*d)->path);
    }
  }
  Status closed = (*d)->fd.Close().WithContext((*d)->path);
  dirs_.erase(dir);
  return closed;
}

Result<std::string> MemRestoreSink::Create(DirHandle parent, std::string_view name, MemNode node) {
  DDS_TRY(CheckName(name));
  auto it = dirs_.find(parent);
  if (it == dirs_.end()) return Status::InvalidArgument("unknown directory handle");
  std::string path = ChildPath(it->second, name);
  if (!nodes_.emplace(path, std::move(node)).second) {
    return Status::AlreadyExists(StrCat(path, ": already exists"));
  }
  return path;
}

Result<RestoreSink::DirHandle> MemRestoreSink::OpenRoot() {
  if (!nodes_.empty()) return Status::AlreadyExists("destination is not empty");
  const DirHandle handle = next_handle_++;
  dirs_.emplace(handle, "");
  return handle;
}

Result<RestoreSink::DirHandle> MemRestoreSink::MakeDir(DirHandle parent, std::string_view name) {
  auto path = Create(parent, name, MemNode{EntryKind::kDirectory, false, 0700, 0, 0, {}, {}});
  DDS_TRY(path.status());
  const DirHandle handle = next_handle_++;
  dirs_.emplace(handle, *path);
  return handle;
}

Result<std::unique_ptr<FileSink>> MemRestoreSink::CreateFile(DirHandle parent,
                                                             std::string_view name) {
  auto path = Create(parent, name, MemNode{EntryKind::kFile, false, 0600, 0, 0, {}, {}});
  DDS_TRY(path.status());
  return std::unique_ptr<FileSink>(std::make_unique<MemFileSink>(&nodes_.at(*path)));
}

Status MemRestoreSink::MakeSymlink(DirHandle parent, std::string_view name, std::string_view target,
                                   const EntryMeta& meta) {
  // The recorded mode is not applied to symlinks (D54); the node keeps 0777 like MemSourceTree's.
  auto path = Create(parent, name,
                     MemNode{EntryKind::kSymlink,
                             false,
                             0777,
                             meta.mtime_sec,
                             meta.mtime_nsec,
                             {},
                             std::string(target)});
  return path.status();
}

Status MemRestoreSink::FinishDir(DirHandle dir, const std::optional<EntryMeta>& meta) {
  auto it = dirs_.find(dir);
  if (it == dirs_.end()) return Status::InvalidArgument("unknown directory handle");
  if (meta.has_value() && !it->second.empty()) {
    MemNode& node = nodes_.at(it->second);
    node.mode = meta->mode;
    node.mtime_sec = meta->mtime_sec;
    node.mtime_nsec = meta->mtime_nsec;
  }
  dirs_.erase(it);
  return Status::Ok();
}

}  // namespace dds
