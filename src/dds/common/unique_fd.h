#pragma once

#include "dds/common/status.h"

namespace dds {

// Owns a POSIX file descriptor and closes it when destroyed. Move-only.
// The destructor ignores close(2) errors because it cannot report them; code that must know
// whether a close succeeded calls Close() first.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() { Reset(); }

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  // Gives up ownership without closing.
  int Release();
  // Closes the current descriptor (ignoring errors) and takes ownership of `fd`.
  void Reset(int fd = -1);
  // Closes the descriptor and reports close(2)'s result. A no-op on an empty UniqueFd. Never
  // retries on EINTR: on Linux the descriptor is already released by then (close(2)).
  Status Close();

 private:
  int fd_ = -1;
};

}  // namespace dds
