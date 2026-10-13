#include "dds/snapshot/source_tree.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>

#include "dds/chunk/fd_byte_source.h"
#include "dds/common/check.h"
#include "dds/common/strings.h"
#include "dds/fs/posix_fs.h"

namespace dds {

namespace {

SourceEntry FromStat(std::string name, const struct stat& st) {
  SourceEntry e;
  e.name = std::move(name);
  if (S_ISREG(st.st_mode)) {
    e.kind = SourceKind::kFile;
  } else if (S_ISDIR(st.st_mode)) {
    e.kind = SourceKind::kDirectory;
  } else if (S_ISLNK(st.st_mode)) {
    e.kind = SourceKind::kSymlink;
  } else {
    e.kind = SourceKind::kOther;
  }
  e.mode = static_cast<uint32_t>(st.st_mode) & 07777;
#if defined(__APPLE__)
  e.mtime_sec = st.st_mtimespec.tv_sec;
  e.mtime_nsec = static_cast<uint32_t>(st.st_mtimespec.tv_nsec);
#else
  e.mtime_sec = st.st_mtim.tv_sec;
  e.mtime_nsec = static_cast<uint32_t>(st.st_mtim.tv_nsec);
#endif
  e.size = e.kind == SourceKind::kFile ? static_cast<uint64_t>(st.st_size) : 0;
  return e;
}

bool ByName(const SourceEntry& a, const SourceEntry& b) { return a.name < b.name; }

class MemByteSource final : public ByteSource {
 public:
  explicit MemByteSource(const std::vector<std::byte>& data) : source_(data) {}
  Result<size_t> Read(std::span<std::byte> out) override { return source_.Read(out); }

 private:
  SpanByteSource source_;
};

}  // namespace

std::string PosixSourceTree::Full(const std::string& path) const {
  return path.empty() ? root_ : root_ + "/" + path;
}

Result<std::vector<SourceEntry>> PosixSourceTree::ListDir(const std::string& path) {
  const std::string dir = Full(path);
  std::unique_ptr<DIR, int (*)(DIR*)> d(::opendir(dir.c_str()), &::closedir);
  if (d == nullptr) return ErrnoStatus(errno, "opendir", dir);
  const int fd = ::dirfd(d.get());
  if (fd < 0) return ErrnoStatus(errno, "dirfd", dir);
  std::vector<SourceEntry> out;
  while (true) {
    errno = 0;
    const struct dirent* entry = ::readdir(d.get());
    if (entry == nullptr) {
      if (errno != 0) return ErrnoStatus(errno, "readdir", dir);
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    // fstatat relative to the open directory with AT_SYMLINK_NOFOLLOW: a symlink is reported as
    // itself, never as its target (backups never follow symlinks, SPEC.md section 4).
    struct stat st{};
    if (::fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
      return ErrnoStatus(errno, "lstat", StrCat(dir, "/", name));
    }
    out.push_back(FromStat(name, st));
  }
  std::sort(out.begin(), out.end(), ByName);
  return out;
}

Result<std::unique_ptr<ByteSource>> PosixSourceTree::OpenFile(const std::string& path) {
  const std::string full = Full(path);
  // O_NOFOLLOW: if the entry was replaced by a symlink since it was listed, fail instead of reading
  // whatever the link points to.
  UniqueFd fd(::open(full.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!fd.valid()) return ErrnoStatus(errno, "open", full);
  return std::unique_ptr<ByteSource>(std::make_unique<FdByteSource>(std::move(fd), full));
}

Result<std::string> PosixSourceTree::ReadLink(const std::string& path) {
  const std::string full = Full(path);
  std::array<char, 4096> buf{};
  const ssize_t n = ::readlink(full.c_str(), buf.data(), buf.size());
  if (n < 0) return ErrnoStatus(errno, "readlink", full);
  // A result that fills the buffer may have been truncated: the target is longer than 4095 bytes,
  // the format's limit (SPEC.md section 6).
  if (static_cast<size_t>(n) >= buf.size()) {
    return Status::InvalidArgument(StrCat(full, ": symlink target is longer than 4095 bytes"));
  }
  return std::string(buf.data(), static_cast<size_t>(n));
}

void MemSourceTree::CheckParent(const std::string& path) const {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return;  // in the root
  auto it = nodes_.find(path.substr(0, slash));
  DDS_CHECK(it != nodes_.end() && it->second.kind == EntryKind::kDirectory);
}

void MemSourceTree::AddDir(const std::string& path, uint32_t mode, int64_t sec, uint32_t nsec) {
  CheckParent(path);
  nodes_[path] = MemNode{EntryKind::kDirectory, false, mode, sec, nsec, {}, {}};
}

void MemSourceTree::AddFile(const std::string& path, std::vector<std::byte> content, uint32_t mode,
                            int64_t sec, uint32_t nsec) {
  CheckParent(path);
  nodes_[path] = MemNode{EntryKind::kFile, false, mode, sec, nsec, std::move(content), {}};
}

void MemSourceTree::AddSymlink(const std::string& path, std::string target, int64_t sec,
                               uint32_t nsec) {
  CheckParent(path);
  nodes_[path] = MemNode{EntryKind::kSymlink, false, 0777, sec, nsec, {}, std::move(target)};
}

void MemSourceTree::AddSpecial(const std::string& path) {
  CheckParent(path);
  nodes_[path] = MemNode{EntryKind::kFile, true, 0600, 0, 0, {}, {}};
}

Result<std::vector<SourceEntry>> MemSourceTree::ListDir(const std::string& path) {
  if (!path.empty()) {
    auto it = nodes_.find(path);
    if (it == nodes_.end() || it->second.kind != EntryKind::kDirectory) {
      return Status::NotFound(StrCat("no directory '", path, "' in the source tree"));
    }
  }
  const std::string prefix = path.empty() ? "" : path + "/";
  std::vector<SourceEntry> out;
  for (auto it = nodes_.lower_bound(prefix); it != nodes_.end() && it->first.starts_with(prefix);
       ++it) {
    const std::string name = it->first.substr(prefix.size());
    if (name.empty() || name.find('/') != std::string::npos) continue;  // not a direct child
    const MemNode& n = it->second;
    SourceEntry e;
    e.name = name;
    e.kind = n.special                         ? SourceKind::kOther
             : n.kind == EntryKind::kFile      ? SourceKind::kFile
             : n.kind == EntryKind::kDirectory ? SourceKind::kDirectory
                                               : SourceKind::kSymlink;
    e.mode = n.mode;
    e.mtime_sec = n.mtime_sec;
    e.mtime_nsec = n.mtime_nsec;
    e.size = n.content.size();
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), ByName);
  return out;
}

Result<std::unique_ptr<ByteSource>> MemSourceTree::OpenFile(const std::string& path) {
  auto it = nodes_.find(path);
  if (it == nodes_.end() || it->second.kind != EntryKind::kFile || it->second.special) {
    return Status::NotFound(StrCat("no file '", path, "' in the source tree"));
  }
  return std::unique_ptr<ByteSource>(std::make_unique<MemByteSource>(it->second.content));
}

Result<std::string> MemSourceTree::ReadLink(const std::string& path) {
  auto it = nodes_.find(path);
  if (it == nodes_.end() || it->second.kind != EntryKind::kSymlink) {
    return Status::NotFound(StrCat("no symlink '", path, "' in the source tree"));
  }
  return it->second.target;
}

}  // namespace dds
