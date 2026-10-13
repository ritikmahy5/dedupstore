#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dds/fs/fs.h"

namespace dds {

// Fs over the POSIX file system API.
//
// Sync() is fdatasync(2) on Linux: it flushes the data and the metadata needed to read it back
// (including the file size), which is all an append-only file written once needs. On macOS it is
// fcntl(F_FULLFSYNC), because Apple's fsync(2) man page says fsync does not make the drive flush
// its cache. SyncDir() opens the directory and calls fsync(2) on Linux, F_FULLFSYNC on macOS,
// where it was checked to succeed on directory descriptors (D23).
class PosixFs final : public Fs {
 public:
  Result<std::unique_ptr<WritableFile>> CreateNew(const std::string& path) override;
  Result<std::unique_ptr<ReadableFile>> OpenRead(const std::string& path) override;
  Status Rename(const std::string& from, const std::string& to) override;
  Status Link(const std::string& existing, const std::string& link) override;
  Status Remove(const std::string& path) override;
  Status CreateDir(const std::string& path) override;
  Status SyncDir(const std::string& dir) override;
  Result<std::vector<std::string>> ListDir(const std::string& dir) override;
  Result<bool> Exists(const std::string& path) override;
};

// Maps an errno value from an operation on `path` to a Status: ENOENT -> NotFound,
// EEXIST -> AlreadyExists, anything else -> IoError. The message names the operation, the path,
// and strerror.
Status ErrnoStatus(int err, const char* op, const std::string& path);

}  // namespace dds
