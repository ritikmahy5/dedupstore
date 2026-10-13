#include "dds/chunk/fd_byte_source.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

#include "dds/fs/posix_fs.h"

namespace dds {

Result<FdByteSource> FdByteSource::Open(const std::string& path) {
  UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd.valid()) return ErrnoStatus(errno, "open", path);
  return FdByteSource(std::move(fd), path);
}

Result<size_t> FdByteSource::Read(std::span<std::byte> out) {
  while (true) {
    const ssize_t n = ::read(fd_.get(), out.data(), out.size());
    if (n >= 0) return static_cast<size_t>(n);
    if (errno != EINTR) return ErrnoStatus(errno, "read", path_);
  }
}

}  // namespace dds
