#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "dds/chunk/chunk_reader.h"
#include "dds/common/unique_fd.h"

namespace dds {

// A ByteSource that reads a user file through its descriptor with read(2). For user data only
// (files being chunked or backed up); repository files go through Fs (D24).
class FdByteSource final : public ByteSource {
 public:
  // `path` is used in error messages.
  FdByteSource(UniqueFd fd, std::string path) : fd_(std::move(fd)), path_(std::move(path)) {}
  // Opens `path` read-only. NotFound if it does not exist.
  static Result<FdByteSource> Open(const std::string& path);

  Result<size_t> Read(std::span<std::byte> out) override;

 private:
  UniqueFd fd_;
  std::string path_;
};

}  // namespace dds
