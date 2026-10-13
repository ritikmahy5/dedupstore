#include "dds/fs/posix_fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dds/common/strings.h"
#include "dds/common/unique_fd.h"

namespace dds {

namespace {

// Makes file data durable (D23).
int SyncFd(int fd) {
#if defined(__APPLE__)
  // Apple's fsync(2) does not make the drive flush its write cache; F_FULLFSYNC does.
  return ::fcntl(fd, F_FULLFSYNC);
#else
  // fdatasync flushes the data plus the metadata needed to read it back, including the size.
  return ::fdatasync(fd);
#endif
}

// Makes directory entries durable (D23). On macOS F_FULLFSYNC was checked to succeed on directory
// descriptors (tests/unit/fs/posix_fs_test.cc re-checks it).
int SyncDirFd(int fd) {
#if defined(__APPLE__)
  return ::fcntl(fd, F_FULLFSYNC);
#else
  return ::fsync(fd);
#endif
}

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(UniqueFd fd, std::string path) : fd_(std::move(fd)), path_(std::move(path)) {}

  Status Append(std::span<const std::byte> data) override {
    const std::byte* p = data.data();
    size_t left = data.size();
    while (left > 0) {
      const ssize_t n = ::write(fd_.get(), p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus(errno, "write", path_);
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
    return Status::Ok();
  }

  Status Sync() override {
    if (SyncFd(fd_.get()) != 0) return ErrnoStatus(errno, "sync", path_);
    return Status::Ok();
  }

  Status Close() override { return fd_.Close().WithContext(path_); }

 private:
  UniqueFd fd_;
  std::string path_;
};

class PosixReadableFile final : public ReadableFile {
 public:
  PosixReadableFile(UniqueFd fd, std::string path) : fd_(std::move(fd)), path_(std::move(path)) {}

  Status ReadAt(uint64_t offset, std::span<std::byte> out) const override {
    std::byte* p = out.data();
    size_t left = out.size();
    uint64_t pos = offset;
    while (left > 0) {
      const ssize_t n = ::pread(fd_.get(), p, left, static_cast<off_t>(pos));
      if (n < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus(errno, "pread", path_);
      }
      if (n == 0) {
        return Status::IoError(StrCat(path_, ": read of ", out.size(), " bytes at offset ", offset,
                                      " extends past the end of the file"));
      }
      p += n;
      pos += static_cast<uint64_t>(n);
      left -= static_cast<size_t>(n);
    }
    if (out.empty()) {
      // A zero-length read is valid at any offset up to the size, like a read of N bytes.
      auto size = Size();
      DDS_TRY(size.status());
      if (offset > *size) {
        return Status::IoError(
            StrCat(path_, ": offset ", offset, " is past the end (size ", *size, ")"));
      }
    }
    return Status::Ok();
  }

  Result<uint64_t> Size() const override {
    struct stat st{};
    if (::fstat(fd_.get(), &st) != 0) return ErrnoStatus(errno, "fstat", path_);
    return static_cast<uint64_t>(st.st_size);
  }

 private:
  UniqueFd fd_;
  std::string path_;
};

}  // namespace

Status ErrnoStatus(int err, const char* op, const std::string& path) {
  std::string message = StrCat(op, " ", path, ": ", std::strerror(err));
  switch (err) {
    case ENOENT:
      return Status::NotFound(std::move(message));
    case EEXIST:
      return Status::AlreadyExists(std::move(message));
    default:
      return Status::IoError(std::move(message));
  }
}

Result<std::unique_ptr<WritableFile>> PosixFs::CreateNew(const std::string& path) {
  UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
  if (!fd.valid()) return ErrnoStatus(errno, "open", path);
  return std::unique_ptr<WritableFile>(std::make_unique<PosixWritableFile>(std::move(fd), path));
}

Result<std::unique_ptr<ReadableFile>> PosixFs::OpenRead(const std::string& path) {
  UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd.valid()) return ErrnoStatus(errno, "open", path);
  return std::unique_ptr<ReadableFile>(std::make_unique<PosixReadableFile>(std::move(fd), path));
}

Status PosixFs::Rename(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) return ErrnoStatus(errno, "rename", from);
  return Status::Ok();
}

Status PosixFs::Link(const std::string& existing, const std::string& link) {
  if (::link(existing.c_str(), link.c_str()) != 0) {
    // Name the path that caused the failure: the target if it exists, else the source.
    return ErrnoStatus(errno, "link", errno == EEXIST ? link : existing);
  }
  return Status::Ok();
}

Status PosixFs::Remove(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return ErrnoStatus(errno, "unlink", path);
  return Status::Ok();
}

Status PosixFs::CreateDir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) != 0) return ErrnoStatus(errno, "mkdir", path);
  return Status::Ok();
}

Status PosixFs::SyncDir(const std::string& dir) {
  UniqueFd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!fd.valid()) return ErrnoStatus(errno, "open", dir);
  if (SyncDirFd(fd.get()) != 0) return ErrnoStatus(errno, "sync directory", dir);
  return fd.Close().WithContext(dir);
}

Result<std::vector<std::string>> PosixFs::ListDir(const std::string& dir) {
  std::unique_ptr<DIR, int (*)(DIR*)> d(::opendir(dir.c_str()), &::closedir);
  if (d == nullptr) return ErrnoStatus(errno, "opendir", dir);
  std::vector<std::string> names;
  while (true) {
    errno = 0;
    const struct dirent* entry = ::readdir(d.get());
    if (entry == nullptr) {
      if (errno != 0) return ErrnoStatus(errno, "readdir", dir);
      break;
    }
    const std::string_view name = entry->d_name;
    if (name == "." || name == "..") continue;
    names.emplace_back(name);
  }
  // readdir order is arbitrary; sorting makes every caller deterministic.
  std::sort(names.begin(), names.end());
  return names;
}

Result<bool> PosixFs::Exists(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0) return true;
  if (errno == ENOENT || errno == ENOTDIR) return false;
  return ErrnoStatus(errno, "lstat", path);
}

}  // namespace dds
