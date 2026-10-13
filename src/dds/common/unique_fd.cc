#include "dds/common/unique_fd.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "dds/common/strings.h"

namespace dds {

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other) Reset(other.Release());
  return *this;
}

int UniqueFd::Release() {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

void UniqueFd::Reset(int fd) {
  if (fd_ >= 0) (void)::close(fd_);
  fd_ = fd;
}

Status UniqueFd::Close() {
  if (fd_ < 0) return Status::Ok();
  const int fd = Release();
  if (::close(fd) != 0) {
    return Status::IoError(StrCat("close(", fd, "): ", std::strerror(errno)));
  }
  return Status::Ok();
}

}  // namespace dds
